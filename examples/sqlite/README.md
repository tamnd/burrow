# A SQLite driver for database/sql

This directory is a database/sql driver for SQLite, written the way any program that uses burrow would write one. It is not part of the library. burrow has no idea it is here, and nothing in `src` links SQLite. The driver links the system's SQLite with `-lsqlite3`, so it shows what it takes to plug a real C library into database/sql.

| File | What it is |
|---|---|
| `sqlite.h` | The driver's interface: `sqlite_driver` and `sqlite_register` |
| `sqlite.c` | The driver, over the sqlite3 C API |
| `demo.c` | A program that uses it through database/sql |
| `sqlite_test.c` | The driver's tests, with burrow's testing package |

## Building

You need SQLite's headers and library. On Debian and Ubuntu that is `apt install libsqlite3-dev`, on Fedora `dnf install sqlite-devel`, and on macOS the system has it, or `brew install sqlite`.

```sh
make sqlite
```

That builds burrow, builds the two programs against it, runs the tests, and checks the demo's output against the comment at the end of `demo.c`. pkg-config finds SQLite when it is installed somewhere unusual. When pkg-config is not there, set the flags yourself:

```sh
make sqlite SQLITE_CFLAGS=-I/opt/sqlite/include SQLITE_LIBS="-L/opt/sqlite/lib -lsqlite3"
```

To use the driver in your own program, compile `sqlite.c` next to it and link both libraries:

```sh
cc -Iinclude -Iexamples/sqlite app.c examples/sqlite/sqlite.c build/libburrow.a -lsqlite3 -lpthread -lm -o app
```

## Using it

```c
sqlite_register();
Error err = BURROW_NO_ERROR;
SqlDB *db = sql_open(BURROW_S("sqlite"), BURROW_S("file:app.db"), &err);
```

The data source name goes straight to `sqlite3_open_v2` with URIs on, so a plain path, `file:app.db?mode=ro` and `:memory:` all work. An in memory database belongs to one connection, and database/sql keeps a pool, so call `sql_db_set_max_open_conns(db, 1)` with `:memory:`, or each connection gets its own empty database.

An exec without arguments runs every statement in the string, which is what a schema file wants. A query with arguments has to be a single statement.

Arguments can be NULL, `int64_t`, `double`, `bool`, `Str`, `Bytes` or `Time`. A placeholder is `?`, `?NNN`, or a name, `:id`, `@id` or `$id`, which an `sql_named` argument binds to. Columns come back as the type SQLite stored, so INTEGER is `int64_t`, REAL is `double`, TEXT is `Str` and BLOB is `Bytes`. There are two exceptions, and both follow github.com/mattn/go-sqlite3. A column declared DATE, DATETIME or TIMESTAMP whose text parses as a time comes back as a `Time` in UTC. A column declared BOOLEAN comes back as a `bool`.

A read only transaction, `SqlTxOptions{.read_only = true}`, turns on SQLite's `query_only` pragma for the length of the transaction. Every isolation level up to serializable is accepted, since SQLite's transactions are serializable. Linearizable is refused.

A context is checked when a statement starts. A statement that is already running finishes even if its context ends.
