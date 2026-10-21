/* database/sql, a generic interface around SQL databases.
 *
 * A program opens a database with the name of a driver that has registered
 * itself and a data source name the driver understands, and gets back a pool
 * of connections it can query from any number of goroutines:
 *
 *     Error err = BURROW_NO_ERROR;
 *     SqlDB *db = sql_open(BURROW_S("sqlite"), BURROW_S("file:app.db"), &err);
 *     ...
 *     SqlRows *rows = sql_db_query_v(db, &err, BURROW_S("SELECT id, name FROM users"
 *                                                      " WHERE age > ?"), 30);
 *     while (sql_rows_next(rows)) {
 *         int64_t id = 0;
 *         Str name = BURROW_STR_EMPTY;
 *         err = sql_rows_scan_v(rows, a, &id, &name);
 *         ...
 *     }
 *     err = sql_rows_err(rows);
 *     sql_rows_free(rows);
 *
 * Everything works as it does in Go: the pool opens connections when it needs
 * them and keeps a few idle ones, a statement prepared on the DB is prepared
 * again on whichever connection runs it, a transaction keeps one connection
 * until it commits or rolls back, and a bad connection is retried on another.
 *
 * Everything here has to run inside runtime_main, because the pool starts
 * goroutines.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package database/sql */

/* Memory. Go's collector decides when a DB, a Conn, a Tx, a Stmt and a Rows go
 * away. Here each of them comes from the heap and is counted, and each has two
 * calls that end it. The _close call is Go's Close, Commit or Rollback, and
 * leaves the object in place, so that sql_rows_err still answers after the
 * Rows is closed and a second close is harmless. The _free call closes it if
 * that has not happened, rolling a Tx back, and lets go of the caller's hold.
 * The memory goes when nothing else holds it either: a Stmt holds its DB and a
 * Rows holds its Stmt, so they can be freed in any order, and a DB freed while
 * a Rows is still open closes when the Rows is freed. Free each one once.
 *
 * What a call is given is borrowed for the call. A query string or argument is
 * copied if it is kept. A context is only watched during the call, except the
 * one given to a query_context, which the Rows watches until it is closed, and
 * the one given to a begin_tx, which the Tx watches until it ends. Those two
 * have to stay valid that long.
 *
 * What comes back is the caller's in two ways. A Result comes from the
 * allocator the exec call is given, since nothing ends it. What a scan stores
 * comes from the allocator the scan is given, so a string scanned from one row
 * stays good after the next, the way it does in Go. SqlRawBytes is the one
 * exception, as in Go: it points into the Rows and is good until the next call
 * to next, scan or close.
 *
 * A query's arguments are a Slice of Any, which the _v macros build from
 * ordinary values the way fmt's do. A value is converted for the driver by the
 * driver's own checks and then by driver_default_parameter_converter, so an
 * integer type becomes an int64_t, a pointer is followed, and a type with a
 * Value method, as described in driver.h, is asked for its value. An
 * SqlNamedArg names an argument and an SqlOut marks one as an output. */

#ifndef BURROW_DATABASE_SQL_H
#define BURROW_DATABASE_SQL_H

#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/database/sql/driver.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/iface.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/time.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- Drivers */

/* sql.Register. Makes driver available to sql_open under name. Panics if
 * driver is nil or if a driver already has the name, with Go's messages. A
 * driver registers once, before it is used, and stays registered for the life
 * of the program. */
void sql_register(Str name, Driver driver);

/* sql.Drivers. The names of the registered drivers, sorted, as a Slice of Str
 * made in a. */
BURROW_OWNS(ret) Slice sql_drivers(Alloc *a);

/* -------------------------------------------------------------- Arguments */

/* sql.NamedArg, an argument that goes to the driver with a name, for a query
 * that uses named parameters such as :id or @id. Pass it as an Any whose type
 * is TYPE_OF(SqlNamedArg), which sql_named_any builds:
 *
 *     SqlNamedArg id = sql_named(BURROW_S("id"), BURROW_ANY_OF(n));
 *     sql_db_query_v(db, &err, BURROW_S("SELECT name FROM users WHERE id = :id"),
 *                    sql_named_any(&id));
 *
 * A name has to begin with a letter. value may itself be an SqlOut. */
typedef struct SqlNamedArg {
    Str name;
    Any value;
} SqlNamedArg;

extern const Type burrow_type_SqlNamedArg;

/* sql.Named. The pair, as a struct. Neither is copied. */
SqlNamedArg sql_named(Str name, Any value);

/* The Any for an argument list. */
BURROW_BORROWS(ret, n) static inline Any sql_named_any(SqlNamedArg *n) {
    return (Any){TYPE_OF(SqlNamedArg), n};
}

/* sql.Out, an argument a stored procedure writes to. dest is where the result
 * goes, an Any the way a scan destination is. in says whether the value dest
 * holds is also passed in. Only a driver that supports output parameters
 * accepts one, in its check_named_value. */
typedef struct SqlOut {
    Any dest;
    bool in;
} SqlOut;

extern const Type burrow_type_SqlOut;

/* sql.IsolationLevel, the isolation a transaction asks for. 0 is the driver's
 * default, and a driver that does not support the level asked for fails the
 * begin. */
typedef Int SqlIsolationLevel;

enum {
    SQL_LEVEL_DEFAULT = 0,
    SQL_LEVEL_READ_UNCOMMITTED = 1,
    SQL_LEVEL_READ_COMMITTED = 2,
    SQL_LEVEL_WRITE_COMMITTED = 3,
    SQL_LEVEL_REPEATABLE_READ = 4,
    SQL_LEVEL_SNAPSHOT = 5,
    SQL_LEVEL_SERIALIZABLE = 6,
    SQL_LEVEL_LINEARIZABLE = 7,
};

/* IsolationLevel.String. "Read Committed" and the rest, and
 * "IsolationLevel(9)" for one Go does not name, which is made in a. */
BURROW_OWNS(ret) Str sql_isolation_level_string(SqlIsolationLevel i, Alloc *a);

/* sql.TxOptions, for begin_tx. read_only asks the driver for a transaction
 * that cannot write, if it can give one. */
typedef struct SqlTxOptions {
    SqlIsolationLevel isolation;
    bool read_only;
} SqlTxOptions;

/* ----------------------------------------------------------------- Scanning */

/* sql.Scanner, a type that knows how to store a value it is scanned from. Like
 * a Valuer, a type implements it with a method in its descriptor, named Scan
 * and with the signature below:
 *
 *     static Error money_scan(Money *m, DriverAllocArg a, Any src) { ... }
 *
 *     #define MONEY_METHODS(M, T) M(T, Scan, money_scan, SQL_SIG_SCAN)
 *
 * src is a DriverValue: nil, an int64_t, a double, a bool, Bytes, a Str or a
 * Time. It belongs to the Rows and is gone after the call, so the method
 * copies what it keeps, using a. An Any whose type is TYPE_OF(SqlScanner),
 * holding the interface value itself, works as well. */
typedef struct SqlScannerVT {
    const Type *self_type;
    Error (*scan)(void *self, Alloc *a, Any src);
} SqlScannerVT;

typedef struct SqlScanner {
    const SqlScannerVT *vt;
    void *data;
} SqlScanner;

extern const Type burrow_type_SqlScanner;

#define SQL_SIG_SCAN(IN, OUT) IN(0, DriverAllocArg) IN(1, Any) OUT(Error)

/* The signature of a Compose method, which Go's convertAssign calls to scan a
 * decimal into a type that can build itself from one. */
#define SQL_SIG_COMPOSE(IN, OUT) IN(0, DriverDecomposed) OUT(Error)

/* sql.RawBytes. A destination that takes the bytes of a column without
 * copying them, so they point into memory the Rows owns and are good until
 * the next call to next, scan or close. Pass it as an Any whose type is
 * TYPE_OF(SqlRawBytes). A row from query_row cannot be scanned into it. */
typedef Bytes SqlRawBytes;

extern const Type burrow_type_SqlRawBytes;

/* The nullable types, which scan from a column that may be NULL and turn back
 * into NULL as an argument. valid is false for NULL. Each has a Scan method
 * and a Value method in its descriptor, so it can be given straight to a scan
 * or a query as an Any of its type:
 *
 *     SqlNullString nick = {0};
 *     err = sql_rows_scan_v(rows, a, sql_null_string_any(&nick));
 *
 * Each _scan is the Scan method, which stores src, copying a string into a,
 * and each _value is the Value method, which gives nil or the value, made in
 * a. */

typedef struct SqlNullString {
    Str string;
    bool valid;
} SqlNullString;

typedef struct SqlNullInt64 {
    int64_t int64;
    bool valid;
} SqlNullInt64;

typedef struct SqlNullInt32 {
    int32_t int32;
    bool valid;
} SqlNullInt32;

typedef struct SqlNullInt16 {
    int16_t int16;
    bool valid;
} SqlNullInt16;

typedef struct SqlNullByte {
    Byte byte;
    bool valid;
} SqlNullByte;

typedef struct SqlNullFloat64 {
    double float64;
    bool valid;
} SqlNullFloat64;

/* bool_ because bool is a macro in C. */
typedef struct SqlNullBool {
    bool bool_;
    bool valid;
} SqlNullBool;

typedef struct SqlNullTime {
    Time time;
    bool valid;
} SqlNullTime;

extern const Type burrow_type_SqlNullString;
extern const Type burrow_type_SqlNullInt64;
extern const Type burrow_type_SqlNullInt32;
extern const Type burrow_type_SqlNullInt16;
extern const Type burrow_type_SqlNullByte;
extern const Type burrow_type_SqlNullFloat64;
extern const Type burrow_type_SqlNullBool;
extern const Type burrow_type_SqlNullTime;

BURROW_OWNS(ret) BURROW_OWNS(ns) Error sql_null_string_scan(SqlNullString *ns, Alloc *a,
                                                            Any value);
BURROW_OWNS(ret) DriverValue sql_null_string_value(SqlNullString ns, Alloc *a,
                                                   Error *err);
BURROW_OWNS(ret) Error sql_null_int64_scan(SqlNullInt64 *n, Alloc *a, Any value);
BURROW_OWNS(ret) DriverValue sql_null_int64_value(SqlNullInt64 n, Alloc *a, Error *err);
BURROW_OWNS(ret) Error sql_null_int32_scan(SqlNullInt32 *n, Alloc *a, Any value);
BURROW_OWNS(ret) DriverValue sql_null_int32_value(SqlNullInt32 n, Alloc *a, Error *err);
BURROW_OWNS(ret) Error sql_null_int16_scan(SqlNullInt16 *n, Alloc *a, Any value);
BURROW_OWNS(ret) DriverValue sql_null_int16_value(SqlNullInt16 n, Alloc *a, Error *err);
BURROW_OWNS(ret) Error sql_null_byte_scan(SqlNullByte *n, Alloc *a, Any value);
BURROW_OWNS(ret) DriverValue sql_null_byte_value(SqlNullByte n, Alloc *a, Error *err);
BURROW_OWNS(ret) Error sql_null_float64_scan(SqlNullFloat64 *n, Alloc *a, Any value);
BURROW_OWNS(ret) DriverValue sql_null_float64_value(SqlNullFloat64 n, Alloc *a,
                                                    Error *err);
BURROW_OWNS(ret) Error sql_null_bool_scan(SqlNullBool *n, Alloc *a, Any value);
BURROW_OWNS(ret) DriverValue sql_null_bool_value(SqlNullBool n, Alloc *a, Error *err);
BURROW_OWNS(ret) Error sql_null_time_scan(SqlNullTime *n, Alloc *a, Any value);
BURROW_OWNS(ret) DriverValue sql_null_time_value(SqlNullTime n, Alloc *a, Error *err);

/* The Anys for a scan or an argument list. */
BURROW_BORROWS(ret, n) static inline Any sql_null_string_any(SqlNullString *n) {
    return (Any){TYPE_OF(SqlNullString), n};
}
BURROW_BORROWS(ret, n) static inline Any sql_null_int64_any(SqlNullInt64 *n) {
    return (Any){TYPE_OF(SqlNullInt64), n};
}
BURROW_BORROWS(ret, n) static inline Any sql_null_int32_any(SqlNullInt32 *n) {
    return (Any){TYPE_OF(SqlNullInt32), n};
}
BURROW_BORROWS(ret, n) static inline Any sql_null_int16_any(SqlNullInt16 *n) {
    return (Any){TYPE_OF(SqlNullInt16), n};
}
BURROW_BORROWS(ret, n) static inline Any sql_null_byte_any(SqlNullByte *n) {
    return (Any){TYPE_OF(SqlNullByte), n};
}
BURROW_BORROWS(ret, n) static inline Any sql_null_float64_any(SqlNullFloat64 *n) {
    return (Any){TYPE_OF(SqlNullFloat64), n};
}
BURROW_BORROWS(ret, n) static inline Any sql_null_bool_any(SqlNullBool *n) {
    return (Any){TYPE_OF(SqlNullBool), n};
}
BURROW_BORROWS(ret, n) static inline Any sql_null_time_any(SqlNullTime *n) {
    return (Any){TYPE_OF(SqlNullTime), n};
}

/* sql.Null[T], for any other type. Go's type parameter is v's type here: v
 * points at the T, and valid says whether it holds a value.
 *
 *     uint32_t id = 0;
 *     SqlNull nid = {BURROW_ANY(TYPE_UINT32, &id), false};
 *     err = sql_rows_scan_v(rows, a, sql_null_any(&nid));
 *
 * Scanning NULL sets the T to its zero value, and scanning anything else
 * converts it into the T the way a scan straight into it would. */
typedef struct SqlNull {
    Any v;
    bool valid;
} SqlNull;

extern const Type burrow_type_SqlNull;

BURROW_OWNS(ret) BURROW_OWNS(n) Error sql_null_scan(SqlNull *n, Alloc *a, Any value);
/* nil when not valid, and otherwise the T run through its own Value method if
 * it has one and then driver_default_parameter_converter. */
BURROW_OWNS(ret) DriverValue sql_null_value(SqlNull n, Alloc *a, Error *err);

BURROW_BORROWS(ret, n) static inline Any sql_null_any(SqlNull *n) {
    return (Any){TYPE_OF(SqlNull), n};
}

/* sql.ConvertAssign. Stores src, a value a driver gave, where dest points,
 * converting it the way a scan does, which the documentation of sql_rows_scan
 * spells out. Anything stored that needs memory, a string or a byte slice,
 * comes from a.
 *
 * For a driver that sets scan_column: pass the scan_ctx it was given, which
 * lets SqlRawBytes point into the Rows, and pass NULL for a, which then means
 * the allocator the program gave sql_rows_scan. Anywhere else pass a zero
 * DriverScanContext and an allocator. */
BURROW_OWNS(ret) BURROW_OWNS(dest) Error sql_convert_assign(Alloc *a,
                                                            DriverScanContext scan_ctx,
                                                            Any dest, DriverValue src);

/* ----------------------------------------------------------------- Errors */

/* sql.ErrNoRows, what a row from query_row returns from its scan when the
 * query found nothing. */
extern const Error sql_err_no_rows;

/* sql.ErrConnDone, for a call on a Conn that has been closed. */
extern const Error sql_err_conn_done;

/* sql.ErrTxDone, for a call on a Tx that has committed or rolled back. */
extern const Error sql_err_tx_done;

/* ----------------------------------------------------------------- Result */

/* sql.Result, what an exec returns: the driver's Result. Unlike Go's, it is
 * not tied to the connection the statement ran on, so its methods do not wait
 * for that connection's lock, and a driver whose Result reads from the
 * connection has to lock it itself. */
typedef DriverResultVT SqlResultVT;
typedef DriverResult SqlResult;

/* --------------------------------------------------------------------- DB */

typedef struct SqlDB SqlDB;
typedef struct SqlConn SqlConn;
typedef struct SqlTx SqlTx;
typedef struct SqlStmt SqlStmt;
typedef struct SqlRows SqlRows;
typedef struct SqlColumnType SqlColumnType;

/* sql.Row, what query_row returns: the error from the query, or the Rows to
 * take the first row from. Scan it once, which also frees the Rows. A row that
 * is never scanned keeps its connection, as it does in Go. */
typedef struct SqlRow {
    Error err;
    SqlRows *rows;
} SqlRow;

/* sql.Open. A pool for the database dsn names, using the driver registered as
 * driver_name. Opens no connection yet; sql_db_ping checks that one can be
 * made. A driver with open_connector is asked for its connector once, here.
 * An unknown driver is Go's error. NULL with *err set on failure, and NULL
 * with no error when there is no memory. */
BURROW_OWNS(ret) SqlDB *sql_open(Str driver_name, Str data_source_name, Error *err);

/* sql.OpenDB. A pool that gets its connections from c, which it closes when
 * the DB closes if c has close. NULL when there is no memory. */
BURROW_OWNS(ret) SqlDB *sql_open_db(DriverConnector c);

/* DB.Close. Closes the idle connections and stops new ones being made. A
 * connection in use closes when it is returned. Calling it twice is fine. */
BURROW_OWNS(ret) Error sql_db_close(SqlDB *db);

/* Closes db if it is open and lets go of it. */
void sql_db_free(SqlDB *db);

/* DB.PingContext and DB.Ping. Gets a connection, opening one if none is idle,
 * and asks the driver to ping it if it can. */
BURROW_OWNS(ret) Error sql_db_ping_context(SqlDB *db, Context ctx);
BURROW_OWNS(ret) Error sql_db_ping(SqlDB *db);

/* DB.SetMaxIdleConns. How many idle connections the pool keeps, 2 to begin
 * with. 0 or less keeps none, and more than the open limit is cut to it. */
void sql_db_set_max_idle_conns(SqlDB *db, Int n);

/* DB.SetMaxOpenConns. How many connections may be open at once, in use or
 * idle. 0 or less, the default, is no limit. */
void sql_db_set_max_open_conns(SqlDB *db, Int n);

/* DB.SetConnMaxLifetime. How long a connection is used before it is closed.
 * 0 or less, the default, is forever. */
void sql_db_set_conn_max_lifetime(SqlDB *db, Duration d);

/* DB.SetConnMaxIdleTime. How long a connection may sit idle before it is
 * closed. 0 or less, the default, is forever. */
void sql_db_set_conn_max_idle_time(SqlDB *db, Duration d);

/* sql.DBStats. */
typedef struct SqlDBStats {
    Int max_open_connections;
    Int open_connections;
    Int in_use;
    Int idle;
    int64_t wait_count;
    Duration wait_duration;
    int64_t max_idle_closed;
    int64_t max_idle_time_closed;
    int64_t max_lifetime_closed;
} SqlDBStats;

/* DB.Stats. */
SqlDBStats sql_db_stats(SqlDB *db);

/* DB.Driver. The driver behind db's connector. */
Driver sql_db_driver(SqlDB *db);

/* DB.PrepareContext and DB.Prepare. A statement for query, prepared on one
 * connection now and on others as it is run on them. ctx is for the prepare,
 * not for the statement's later use. */
BURROW_OWNS(ret) SqlStmt *sql_db_prepare_context(SqlDB *db, Context ctx, Str query,
                                                 Error *err);
BURROW_OWNS(ret) SqlStmt *sql_db_prepare(SqlDB *db, Str query, Error *err);

/* DB.ExecContext and DB.Exec. Runs a query that returns no rows. args is a
 * Slice of Any. The Result comes from a. */
BURROW_OWNS(ret) SqlResult sql_db_exec_context(SqlDB *db, Alloc *a, Context ctx,
                                               Str query, Slice args, Error *err);
BURROW_OWNS(ret) SqlResult sql_db_exec(SqlDB *db, Alloc *a, Str query, Slice args,
                                       Error *err);

/* DB.QueryContext and DB.Query. Runs a query that returns rows. The Rows keeps
 * a connection until it is closed. */
BURROW_OWNS(ret) SqlRows *sql_db_query_context(SqlDB *db, Context ctx, Str query,
                                               Slice args, Error *err);
BURROW_OWNS(ret) SqlRows *sql_db_query(SqlDB *db, Str query, Slice args, Error *err);

/* DB.QueryRowContext and DB.QueryRow. A query expected to return at most one
 * row. An error is kept for the scan. */
SqlRow sql_db_query_row_context(SqlDB *db, Context ctx, Str query, Slice args);
SqlRow sql_db_query_row(SqlDB *db, Str query, Slice args);

/* DB.BeginTx and DB.Begin. Starts a transaction, which keeps a connection
 * until it commits or rolls back, and rolls back if ctx is cancelled first.
 * opts may be NULL for the defaults. */
BURROW_OWNS(ret) SqlTx *sql_db_begin_tx(SqlDB *db, Context ctx,
                                        const SqlTxOptions *opts, Error *err);
BURROW_OWNS(ret) SqlTx *sql_db_begin(SqlDB *db, Error *err);

/* DB.Conn. One connection from the pool, kept until it is closed, for work
 * that has to happen on the same connection. */
BURROW_OWNS(ret) SqlConn *sql_db_conn(SqlDB *db, Context ctx, Error *err);

/* ------------------------------------------------------------------- Conn */

/* sql.Conn. The same calls as the DB's, all on the one connection. */
BURROW_OWNS(ret) Error sql_conn_ping_context(SqlConn *c, Context ctx);
BURROW_OWNS(ret) SqlResult sql_conn_exec_context(SqlConn *c, Alloc *a, Context ctx,
                                                 Str query, Slice args, Error *err);
BURROW_OWNS(ret) SqlRows *sql_conn_query_context(SqlConn *c, Context ctx, Str query,
                                                 Slice args, Error *err);
SqlRow sql_conn_query_row_context(SqlConn *c, Context ctx, Str query, Slice args);
BURROW_OWNS(ret) SqlStmt *sql_conn_prepare_context(SqlConn *c, Context ctx, Str query,
                                                   Error *err);
BURROW_OWNS(ret) SqlTx *sql_conn_begin_tx(SqlConn *c, Context ctx,
                                          const SqlTxOptions *opts, Error *err);

/* What Conn.Raw takes, called with the driver's connection. */
BURROW_FUNC(SqlRawFunc, Error, DriverConn driver_conn);

/* Conn.Raw. Calls f with the driver's connection, holding the connection's
 * lock, for driver features database/sql has no call for. f must not keep the
 * connection after it returns. */
BURROW_OWNS(ret) Error sql_conn_raw(SqlConn *c, SqlRawFunc f);

/* Conn.Close. Returns the connection to the pool. Calls after it return
 * sql_err_conn_done, and so does a second close. */
BURROW_OWNS(ret) Error sql_conn_close(SqlConn *c);

/* Closes c if it is open and lets go of it. */
void sql_conn_free(SqlConn *c);

/* --------------------------------------------------------------------- Tx */

/* Tx.Commit and Tx.Rollback. Each ends the transaction and closes the
 * statements prepared in it. Either one after the end returns sql_err_tx_done.
 */
BURROW_OWNS(ret) Error sql_tx_commit(SqlTx *tx);
BURROW_OWNS(ret) Error sql_tx_rollback(SqlTx *tx);

/* Rolls tx back if it has not ended and lets go of it. It first waits for the
 * goroutine that rolls tx back when its context ends, which the commit or
 * rollback has woken by then. */
void sql_tx_free(SqlTx *tx);

/* The DB's calls, inside the transaction. */
BURROW_OWNS(ret) SqlStmt *sql_tx_prepare_context(SqlTx *tx, Context ctx, Str query,
                                                 Error *err);
BURROW_OWNS(ret) SqlStmt *sql_tx_prepare(SqlTx *tx, Str query, Error *err);
BURROW_OWNS(ret) SqlResult sql_tx_exec_context(SqlTx *tx, Alloc *a, Context ctx,
                                               Str query, Slice args, Error *err);
BURROW_OWNS(ret) SqlResult sql_tx_exec(SqlTx *tx, Alloc *a, Str query, Slice args,
                                       Error *err);
BURROW_OWNS(ret) SqlRows *sql_tx_query_context(SqlTx *tx, Context ctx, Str query,
                                               Slice args, Error *err);
BURROW_OWNS(ret) SqlRows *sql_tx_query(SqlTx *tx, Str query, Slice args, Error *err);
SqlRow sql_tx_query_row_context(SqlTx *tx, Context ctx, Str query, Slice args);
SqlRow sql_tx_query_row(SqlTx *tx, Str query, Slice args);

/* Tx.StmtContext and Tx.Stmt. stmt, which was prepared on the DB, for use in
 * the transaction. It closes when the transaction ends. A failure is kept in
 * the statement and returned by each call on it. NULL only when there is no
 * memory. */
BURROW_OWNS(ret) SqlStmt *sql_tx_stmt_context(SqlTx *tx, Context ctx, SqlStmt *stmt);
BURROW_OWNS(ret) SqlStmt *sql_tx_stmt(SqlTx *tx, SqlStmt *stmt);

/* ------------------------------------------------------------------- Stmt */

/* The calls of a prepared statement, with only the arguments. */
BURROW_OWNS(ret) SqlResult sql_stmt_exec_context(SqlStmt *s, Alloc *a, Context ctx,
                                                 Slice args, Error *err);
BURROW_OWNS(ret) SqlResult sql_stmt_exec(SqlStmt *s, Alloc *a, Slice args, Error *err);
BURROW_OWNS(ret) SqlRows *sql_stmt_query_context(SqlStmt *s, Context ctx, Slice args,
                                                 Error *err);
BURROW_OWNS(ret) SqlRows *sql_stmt_query(SqlStmt *s, Slice args, Error *err);
SqlRow sql_stmt_query_row_context(SqlStmt *s, Context ctx, Slice args);
SqlRow sql_stmt_query_row(SqlStmt *s, Slice args);

/* Stmt.Close. Calling it twice is fine. */
BURROW_OWNS(ret) Error sql_stmt_close(SqlStmt *s);

/* Closes s if it is open and lets go of it. */
void sql_stmt_free(SqlStmt *s);

/* ------------------------------------------------------------------- Rows */

/* Rows.Next. Moves to the next row, and answers whether there is one. At the
 * end, or on an error, the Rows closes itself and sql_rows_err tells which. */
bool sql_rows_next(SqlRows *rs);

/* Rows.NextResultSet. Moves to the next result set of a query that returned
 * more than one, after which sql_rows_next gives its first row. False when
 * there is none, and then the Rows is closed. */
bool sql_rows_next_result_set(SqlRows *rs);

/* Rows.Err. The error that ended the rows, if there was one other than
 * reaching the end. Good after close. */
BURROW_BORROWS(ret, rs) Error sql_rows_err(SqlRows *rs);

/* Rows.Columns. The column names, a Slice of Str, copied into a. An error once
 * the Rows is closed. */
BURROW_OWNS(ret) Slice sql_rows_columns(SqlRows *rs, Alloc *a, Error *err);

/* Rows.ColumnTypes. What the driver says about each column, a Slice of
 * SqlColumnType *, made in a. */
BURROW_OWNS(ret) Slice sql_rows_column_types(SqlRows *rs, Alloc *a, Error *err);

/* Rows.Scan. Stores the current row's columns where dest, a Slice of Any, says,
 * one destination per column, and converts each value to its destination's
 * type. Go's rules, by destination:
 *
 *   Str, Bytes, bool, the integers, the floats and Time: converted, a number
 *   to and from its text, failing when it would lose information. A string or
 *   bytes are copied into a.
 *   Any: the value as the driver gave it, copied into a.
 *   SqlRawBytes: the bytes, not copied, good until the next next.
 *   A type with a Scan method: the method, given the value.
 *   A pointer type: NULL for NULL, and otherwise a new value from a.
 *   Uuid: from its text or its sixteen bytes.
 *   SqlRowsPtr: a nested cursor, as a new Rows the caller frees.
 *
 * Storing NULL anywhere but an Any, Bytes, a pointer or a Scanner is an error.
 * The _v form takes the destinations as pointers, the way fmt_sscan_v does.
 */
BURROW_OWNS(ret) BURROW_OWNS(dest) Error sql_rows_scan(SqlRows *rs, Alloc *a,
                                                       Slice dest);

/* Rows.Close. Ends the query and returns its connection. Calling it twice is
 * fine. */
BURROW_OWNS(ret) Error sql_rows_close(SqlRows *rs);

/* Closes rs if it is open and lets go of it. It first waits for the goroutine
 * that closes rs when its context ends, which the close has woken by then. */
void sql_rows_free(SqlRows *rs);

/* A pointer to a Rows, as a scan destination for a driver that returns a
 * cursor as a column's value. */
typedef SqlRows *SqlRowsPtr;
extern const Type burrow_type_SqlRowsPtr;

/* --------------------------------------------------------------------- Row */

/* Row.Scan. The first row's columns, as sql_rows_scan stores them, or
 * sql_err_no_rows when there is no row. Frees the Rows, after which a second
 * scan returns sql_err_no_rows. A destination cannot be SqlRawBytes. */
BURROW_OWNS(ret) BURROW_OWNS(dest) Error sql_row_scan(SqlRow *r, Alloc *a, Slice dest);

/* Row.Err. The query's error, without scanning. */
BURROW_BORROWS(ret, r) Error sql_row_err(const SqlRow *r);

/* ------------------------------------------------------------ ColumnType */

/* ColumnType.Name. */
BURROW_BORROWS(ret, ct) Str sql_column_type_name(const SqlColumnType *ct);

/* ColumnType.Length. A variable length column's length, such as a text or
 * blob column's, with ok false for a type that has none or a driver that does
 * not say. */
int64_t sql_column_type_length(const SqlColumnType *ct, bool *ok);

/* ColumnType.DecimalSize. A decimal column's precision, and its scale in
 * *scale, with ok false when the driver does not say. */
int64_t sql_column_type_decimal_size(const SqlColumnType *ct, int64_t *scale, bool *ok);

/* ColumnType.ScanType. The type a column scans into without loss, or Any's
 * descriptor when the driver does not say. */
BURROW_STATIC(ret) const Type *sql_column_type_scan_type(const SqlColumnType *ct);

/* ColumnType.Nullable. Whether the column may be NULL, with ok false when the
 * driver does not say. */
bool sql_column_type_nullable(const SqlColumnType *ct, bool *ok);

/* ColumnType.DatabaseTypeName. The database's name for the type, such as
 * "VARCHAR", in upper case, or "" when the driver does not say. */
BURROW_BORROWS(ret, ct) Str sql_column_type_database_type_name(const SqlColumnType *ct);

/* ----------------------------------------------------------- the _v forms
 *
 * The query arguments as ordinary values, each one boxed with BURROW_ANY_OF,
 * and the scan destinations as pointers, each one boxed with BURROW_SCAN_OF.
 * An Any passes through either unchanged, which is how an SqlNamedArg, a
 * nullable type or any other type with a descriptor goes in. A query with no
 * arguments takes the plain function and an empty Slice. */

#define sql_db_exec_v(db, a, err, query, ...)                                          \
    sql_db_exec((db), (a), (query), BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__), (err))
#define sql_db_query_v(db, err, query, ...)                                            \
    sql_db_query((db), (query), BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__), (err))
#define sql_db_query_row_v(db, query, ...)                                             \
    sql_db_query_row((db), (query), BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__))
#define sql_tx_exec_v(tx, a, err, query, ...)                                          \
    sql_tx_exec((tx), (a), (query), BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__), (err))
#define sql_tx_query_v(tx, err, query, ...)                                            \
    sql_tx_query((tx), (query), BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__), (err))
#define sql_tx_query_row_v(tx, query, ...)                                             \
    sql_tx_query_row((tx), (query), BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__))
#define sql_stmt_exec_v(s, a, err, ...)                                                \
    sql_stmt_exec((s), (a), BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__), (err))
#define sql_stmt_query_v(s, err, ...)                                                  \
    sql_stmt_query((s), BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__), (err))
#define sql_stmt_query_row_v(s, ...)                                                   \
    sql_stmt_query_row((s), BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__))
#define sql_rows_scan_v(rs, a, ...)                                                    \
    sql_rows_scan((rs), (a), BURROW__FMT_ARGS(BURROW_SCAN_OF, __VA_ARGS__))
#define sql_row_scan_v(r, a, ...)                                                      \
    sql_row_scan((r), (a), BURROW__FMT_ARGS(BURROW_SCAN_OF, __VA_ARGS__))

#ifdef __cplusplus
}
#endif

#endif /* BURROW_DATABASE_SQL_H */
