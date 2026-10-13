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

#endif /* BURROW_DATABASE_SQL_INTERNAL_H */
