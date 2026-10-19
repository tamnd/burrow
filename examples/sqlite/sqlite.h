/* sqlite, a database/sql driver for SQLite.
 *
 * This is not part of burrow. It is a program that uses burrow and links one
 * more C library, the SQLite that the system has, the way any C program does:
 * an #include of <sqlite3.h> and -lsqlite3 on the link line. Nothing in burrow
 * knows it is there.
 *
 *     sqlite_register();
 *     SqlDB *db = sql_open(BURROW_S("sqlite"), BURROW_S("file:app.db"), &err);
 *
 * The data source name goes to sqlite3_open_v2 as it is, with URIs turned on,
 * so a path, "file:app.db?mode=ro" and ":memory:" all work. An in memory
 * database belongs to one connection, and database/sql keeps a pool of them,
 * so with ":memory:" set the pool to one connection with
 * sql_db_set_max_open_conns, or each connection sees a database of its own.
 *
 * Values. An argument can be anything the default converter gives: NULL,
 * int64_t, double, bool, Str, Bytes or Time. A bool is stored as 0 or 1 and a
 * Time as text, "2006-01-02 15:04:05.999999999-07:00". A column comes back as
 * what SQLite has in it, so INTEGER is int64_t, REAL is double, TEXT is Str and
 * BLOB is Bytes, except that a column declared DATE, DATETIME or TIMESTAMP
 * whose text parses as a time comes back as a Time in UTC, and one declared
 * BOOLEAN comes back as a bool. That is close to what github.com/mattn/go-sqlite3
 * does, so a schema moves between the two without surprises.
 *
 * Arguments are named or not. One without a name binds to the placeholder at
 * its position, ? or ?NNN, and one made with sql_named binds to :name, @name or
 * $name, whichever the statement has.
 *
 * A context is looked at when a statement starts, and a statement that is
 * already running is not interrupted when the context ends.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_EXAMPLES_SQLITE_H
#define BURROW_EXAMPLES_SQLITE_H

#include "burrow/database/sql/driver.h"

/* The driver. */
extern const Driver sqlite_driver;

/* Registers sqlite_driver with database/sql as "sqlite". Like sql_register, it
 * panics when called a second time. */
void sqlite_register(void);

#endif /* BURROW_EXAMPLES_SQLITE_H */
