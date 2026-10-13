/* What the two halves of database/sql share: sql_convert.c, which is Go's
 * convert.go and the small types, and sql.c, which is the pool and everything
 * on it.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package database/sql */

#ifndef BURROW_DATABASE_SQL_INTERNAL_H
#define BURROW_DATABASE_SQL_INTERNAL_H

#include "burrow/database/sql.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

/* The sentinels both halves return, errNilPtr from convert.go and the
 * unexported ones from sql.go. */
extern const Error sql_err_nil_ptr;
extern const Error burrow__sql_err_db_closed;
extern const Error burrow__sql_err_rows_closed;

/* Rows' own descriptor, which a DriverScanContext carries so that
 * sql_convert_assign can find the Rows it was called for. */
extern const Type burrow__sql_rows_type;

/* Go's Rows.rawbuf and Rows.setrawbuf. The buffer RawBytes are appended to,
 * empty at the start of each scan, and where it went after an append, which
 * returns the part just appended. With no Rows the buffer is nil and an append
 * comes from the scan's allocator, as in Go. raw_alloc is the allocator an
 * append to the buffer uses. */
BURROW_BORROWS(ret, rs) Bytes burrow__sql_rows_rawbuf(SqlRows *rs);
BURROW_BORROWS(ret, b) Bytes burrow__sql_rows_setrawbuf(SqlRows *rs, Bytes b);
Alloc *burrow__sql_rows_raw_alloc(SqlRows *rs, Alloc *a);

/* The allocator the program gave the sql_rows_scan that is calling the
 * driver's scan_column, or NULL outside one. */
Alloc *burrow__sql_rows_scan_alloc(SqlRows *rs);

/* A driver Rows that came back as a column's value, as a Rows of its own on
 * parent's connection, which parent closes when it closes. NULL with *err set
 * when there is no memory. */
BURROW_OWNS(ret) SqlRows *burrow__sql_rows_cursor(SqlRows *parent, DriverRows rows,
                                                  Error *err);

/* convertAssignRows. rows may be NULL. */
BURROW_OWNS(dest) Error burrow__sql_convert_assign_rows(Alloc *a, Any dest, Any src,
                                                        SqlRows *rows);

/* The driver registered under name, from the registry sql_register fills. */
Driver burrow__sql_driver_lookup(Str name, bool *ok);

/* driverArgsConnLocked. Turns a query's arguments, a Slice of Any, into a
 * Slice of DriverNamedValue made in a, the way the driver's checks and its
 * statement's column converters say. si is the statement, or NULL when there
 * is none. The connection has to be locked. */
BURROW_OWNS(ret) Slice burrow__sql_driver_args_conn_locked(Alloc *a, DriverConn ci,
                                                           const DriverStmt *si,
                                                           Slice args, Error *err);

/* hookTxGrabConn, rollbackHook, bypassRowsAwaitDone and rowsCloseHook, which
 * only tests set. */
typedef void (*burrow__SqlHook)(void);
typedef void (*burrow__SqlRowsCloseHook)(SqlRows *rs, Error *err);
extern burrow__SqlHook burrow__sql_hook_tx_grab_conn;
extern burrow__SqlHook burrow__sql_rollback_hook;
extern bool burrow__sql_bypass_rows_await_done;
extern burrow__SqlRowsCloseHook burrow__sql_rows_close_hook;

/* For the tests, which in Go read these straight out of the structs. */
Int burrow__sql_db_num_open(SqlDB *db);
Int burrow__sql_db_num_free(SqlDB *db);
DriverConn burrow__sql_db_free_conn(SqlDB *db, Int i);
Int burrow__sql_db_num_waiting(SqlDB *db);
int64_t burrow__sql_db_num_closed(SqlDB *db);
Int burrow__sql_db_num_deps(SqlDB *db);
/* db.conn, db.putConn and driverConn.ci, with the driverConn kept opaque.
 * always_new is alwaysNewConn rather than cachedOrNewConn. */
void *burrow__sql_db_conn(SqlDB *db, Context ctx, bool always_new, Error *err);
void burrow__sql_db_put_conn(void *dc, Error err, bool reset_session);
DriverConn burrow__sql_dc_driver(void *dc);
/* stmt.css = nil, which makes the next use prepare again. */
void burrow__sql_stmt_force_prepare(SqlStmt *s);
/* One of db.freeConn with a reference taken, which burrow__sql_dc_unref gives
 * back, so that a test can look at it after the pool has let go of it. */
void *burrow__sql_db_free_dc(SqlDB *db, Int i);
void burrow__sql_dc_unref(void *dc);
bool burrow__sql_dc_closed(void *dc);
Int burrow__sql_dc_num_open_stmt(void *dc);
bool burrow__sql_dc_has_driver(void *dc);
Int burrow__sql_stmt_num_css(SqlStmt *s);
DriverConn burrow__sql_conn_driver(SqlConn *c);
DriverConn burrow__sql_tx_driver(SqlTx *tx);
DriverRows burrow__sql_rows_driver(SqlRows *rs);
DriverConnector burrow__sql_db_connector(SqlDB *db);
/* connRequestSet, on a set of its own. add makes the waiter, which take_random
 * and close_all leave for burrow__sql_waiter_free. */
BURROW_OWNS(ret) void *burrow__sql_reqs_new(void);
void burrow__sql_reqs_free(void *set);
Int burrow__sql_reqs_len(void *set);
BURROW_OWNS(ret) void *burrow__sql_reqs_add(void *set);
bool burrow__sql_reqs_delete(void *set, void *w);
void *burrow__sql_reqs_take_random(void *set);
void burrow__sql_reqs_close_all(void *set);
void burrow__sql_waiter_free(void *w);
/* Conn.grabConn and then its release(nil). */
Error burrow__sql_conn_grab_release(SqlConn *c);
/* Stmt.Close on a Stmt made by hand: sticky, or a Tx statement on si. */
Error burrow__sql_stmt_close_detached(Error sticky, DriverStmt si);
/* Rows.closemuScanHold. */
bool burrow__sql_rows_scan_hold(SqlRows *rs);
bool burrow__sql_tx_is_done(SqlTx *tx);
void *burrow__sql_rows_dc(SqlRows *rs);
/* connectionCleanerRunLocked under db.mu, which closes what it finds. ordered
 * reports whether freeConn is still in returnedAt order afterwards. */
Duration burrow__sql_db_cleaner_run(SqlDB *db, Duration d, Int *nclosed, bool *ordered);
Int burrow__sql_db_free_conn_open_stmts(SqlDB *db, Int i);
bool burrow__sql_rows_closed(SqlRows *rs);
void burrow__sql_tx_set_keep_conn_on_rollback(SqlTx *tx, bool keep);
bool burrow__sql_stmt_closed(SqlStmt *s);
Error burrow__sql_stmt_sticky_err(SqlStmt *s);
SqlStmt *burrow__sql_stmt_parent(SqlStmt *s);

#endif /* BURROW_DATABASE_SQL_INTERNAL_H */
