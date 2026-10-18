/* sqlite, a database/sql driver for SQLite. See sqlite.h.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "sqlite.h"

#include <sqlite3.h>
#include <string.h>

#include "burrow/burrow.h"

/* How long a connection waits for another one's lock before it gives up with
 * "database is locked". The pool makes several connections to one file, and
 * without this two writers fail at once instead of taking turns. */
enum { LITE_BUSY_TIMEOUT_MS = 5000 };

/* The layout a Time is written in, and the ones a time column is read with,
 * in mattn/go-sqlite3's order. */
#define LITE_TIME_FORMAT "2006-01-02 15:04:05.999999999-07:00"

static const char *const lite_time_formats[] = {
    "2006-01-02 15:04:05.999999999-07:00",
    "2006-01-02T15:04:05.999999999-07:00",
    "2006-01-02 15:04:05.999999999",
    "2006-01-02T15:04:05.999999999",
    "2006-01-02 15:04:05",
    "2006-01-02T15:04:05",
    "2006-01-02 15:04",
    "2006-01-02T15:04",
    "2006-01-02",
};

typedef struct LiteConn {
    sqlite3 *db;
    bool read_only_tx; /* query_only is on for the transaction */
} LiteConn;

typedef struct LiteStmt {
    LiteConn *c;
    sqlite3_stmt *s; /* NULL for a statement that is only a comment */
    bool rows_open;  /* a Rows is reading from s */
    bool closed;     /* close came while it was, so the Rows finalizes s */
} LiteStmt;

typedef struct LiteRows {
    LiteStmt *st;
    Arena names; /* the column names and type names, until close */
    Arena row;   /* the values of the current row, until the next next */
    Slice cols;  /* Str */
    Str *decl;   /* the declared types, upper case and without a length */
    Int ncols;
} LiteRows;

typedef struct LiteResult {
    int64_t id;
    int64_t changes;
} LiteResult;

static Error lite_error(sqlite3 *db) {
    return fmt_errorf_v("sqlite: %s", str_from_cstr(sqlite3_errmsg(db)));
}

static int64_t lite_changes(sqlite3 *db) {
#if SQLITE_VERSION_NUMBER >= 3037000
    return (int64_t)sqlite3_changes64(db);
#else
    return (int64_t)sqlite3_changes(db);
#endif
}

/* ------------------------------------------------------------------ Result */

static int64_t lite_result_last_insert_id(void *self, Error *err) {
    *err = BURROW_NO_ERROR;
    return ((LiteResult *)self)->id;
}

static int64_t lite_result_rows_affected(void *self, Error *err) {
    *err = BURROW_NO_ERROR;
    return ((LiteResult *)self)->changes;
}

static const DriverResultVT lite_result_vt = {
    .last_insert_id = lite_result_last_insert_id,
    .rows_affected = lite_result_rows_affected,
};

static DriverResult lite_result(Alloc *a, sqlite3 *db, Error *err) {
    LiteResult *r = BURROW_NEW(a, LiteResult);
    if (r == NULL) {
        *err = burrow_err_out_of_memory;
        return (DriverResult){NULL, NULL};
    }
    r->id = (int64_t)sqlite3_last_insert_rowid(db);
    r->changes = lite_changes(db);
    return (DriverResult){&lite_result_vt, r};
}

/* -------------------------------------------------------------------- bind */

/* The placeholder a named argument binds to. SQLite keeps the prefix as part
 * of the name, so each of the three is tried. */
static int lite_named_index(Alloc *a, sqlite3_stmt *s, Str name) {
    static const char prefixes[] = ":@$";
    char *c = mem_alloc(a, (size_t)name.len + 2, 1);
    if (c == NULL)
        return 0;
    memcpy(c + 1, name.p, (size_t)name.len);
    c[name.len + 1] = '\0';
    for (int i = 0; i < 3; i++) {
        c[0] = prefixes[i];
        int idx = sqlite3_bind_parameter_index(s, c);
        if (idx > 0)
            return idx;
    }
    return 0;
}

static int lite_bind_one(Alloc *a, sqlite3_stmt *s, int idx, Any v) {
    if (v.t == NULL)
        return sqlite3_bind_null(s, idx);
    if (v.t == TYPE_INT64)
        return sqlite3_bind_int64(s, idx, *(const int64_t *)v.data);
    if (v.t == TYPE_FLOAT64)
        return sqlite3_bind_double(s, idx, *(const double *)v.data);
    if (v.t == TYPE_BOOL)
        return sqlite3_bind_int(s, idx, *(const bool *)v.data ? 1 : 0);
    if (v.t == TYPE_STRING) {
        Str str = *(const Str *)v.data;
        return sqlite3_bind_text64(s, idx, (const char *)str.p, (sqlite3_uint64)str.len,
                                   SQLITE_TRANSIENT, SQLITE_UTF8);
    }
    if (v.t == TYPE_BYTES) {
        Bytes b = *(const Bytes *)v.data;
        if (b.p == NULL)
            return sqlite3_bind_null(s, idx);
        if (b.len == 0)
            return sqlite3_bind_zeroblob(s, idx, 0);
        return sqlite3_bind_blob64(s, idx, b.p, (sqlite3_uint64)b.len,
                                   SQLITE_TRANSIENT);
    }
    if (v.t == TYPE_TIME) {
        Str str = time_format(*(const Time *)v.data, a, BURROW_S(LITE_TIME_FORMAT));
        return sqlite3_bind_text64(s, idx, (const char *)str.p, (sqlite3_uint64)str.len,
                                   SQLITE_TRANSIENT, SQLITE_UTF8);
    }
    return SQLITE_MISMATCH;
}

/* Binds args, a Slice of DriverNamedValue, to s, after clearing what the last
 * run left. */
static Error lite_bind(LiteStmt *st, Slice args) {
    sqlite3_stmt *s = st->s;
    (void)sqlite3_reset(s);
    (void)sqlite3_clear_bindings(s);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    const DriverNamedValue *nv = args.p;
    for (Int i = 0; i < args.len && BURROW_OK(err); i++) {
        int idx = (int)nv[i].ordinal;
        if (nv[i].name.len > 0) {
            idx = lite_named_index(a, s, nv[i].name);
            if (idx == 0) {
                err = fmt_errorf_v("sqlite: no parameter named %q", nv[i].name);
                break;
            }
        }
        int rc = lite_bind_one(a, s, idx, nv[i].value);
        if (rc == SQLITE_MISMATCH)
            err = fmt_errorf_v("sqlite: argument %d has a type it cannot bind",
                               nv[i].ordinal);
        else if (rc != SQLITE_OK)
            err = lite_error(st->c->db);
    }
    arena_free(&ar);
    return err;
}

/* -------------------------------------------------------------------- Rows */

static Error lite_rows_close(void *self) {
    LiteRows *r = self;
    LiteStmt *st = r->st;
    st->rows_open = false;
    if (st->closed) {
        (void)sqlite3_finalize(st->s);
        mem_free(heap_allocator(), st, sizeof *st, _Alignof(LiteStmt));
    } else {
        (void)sqlite3_reset(st->s);
    }
    arena_free(&r->row);
    arena_free(&r->names);
    mem_free(heap_allocator(), r, sizeof *r, _Alignof(LiteRows));
    return BURROW_NO_ERROR;
}

static Slice lite_rows_columns(void *self) {
    return ((LiteRows *)self)->cols;
}

/* A time column's text as a Time, or false when it is not one. The time is
 * moved to UTC, since a zone parsed from the text would live in the row's
 * arena, and a scanned Time outlives the row. */
static bool lite_parse_time(Alloc *a, Str s, Time *t) {
    s = strings_trim_suffix(s, BURROW_S("Z"));
    for (size_t i = 0; i < sizeof lite_time_formats / sizeof lite_time_formats[0];
         i++) {
        Error err = BURROW_NO_ERROR;
        Time v = time_parse(a, str_from_cstr(lite_time_formats[i]), s, &err);
        if (BURROW_OK(err)) {
            *t = time_utc(v);
            return true;
        }
    }
    return false;
}

static bool lite_is_time_type(Str decl) {
    return str_eq(decl, BURROW_S("DATE")) || str_eq(decl, BURROW_S("DATETIME")) ||
           str_eq(decl, BURROW_S("TIMESTAMP"));
}

/* One column of the current row, in memory from a. */
static Any lite_column(LiteRows *r, Alloc *a, int i, Error *err) {
    sqlite3_stmt *s = r->st->s;
    Str decl = r->decl[i];
    switch (sqlite3_column_type(s, i)) {
    case SQLITE_INTEGER: {
        int64_t n = (int64_t)sqlite3_column_int64(s, i);
        if (str_eq(decl, BURROW_S("BOOLEAN"))) {
            bool *b = BURROW_NEW(a, bool);
            if (b == NULL)
                break;
            *b = n != 0;
            return (Any){TYPE_BOOL, b};
        }
        int64_t *p = BURROW_NEW(a, int64_t);
        if (p == NULL)
            break;
        *p = n;
        return (Any){TYPE_INT64, p};
    }
    case SQLITE_FLOAT: {
        double *p = BURROW_NEW(a, double);
        if (p == NULL)
            break;
        *p = sqlite3_column_double(s, i);
        return (Any){TYPE_FLOAT64, p};
    }
    case SQLITE_TEXT: {
        const unsigned char *text = sqlite3_column_text(s, i);
        Int n = (Int)sqlite3_column_bytes(s, i);
        Str str = str_clone(a, (Str){(const Byte *)text, n});
        if (n > 0 && str.p == NULL)
            break;
        if (lite_is_time_type(decl)) {
            Time *t = BURROW_NEW(a, Time);
            if (t == NULL)
                break;
            if (lite_parse_time(a, str, t))
                return (Any){TYPE_TIME, t};
        }
        Str *p = BURROW_NEW(a, Str);
        if (p == NULL)
            break;
        *p = str;
        return (Any){TYPE_STRING, p};
    }
    case SQLITE_BLOB: {
        const void *blob = sqlite3_column_blob(s, i);
        Int n = (Int)sqlite3_column_bytes(s, i);
        Bytes *p = BURROW_NEW(a, Bytes);
        if (p == NULL)
            break;
        *p = bytes_clone(a, slice_from((void *)(uintptr_t)blob, n, n, TYPE_BYTE));
        if (n > 0 && p->p == NULL)
            break;
        return (Any){TYPE_BYTES, p};
    }
    default: /* SQLITE_NULL */
        return (Any){NULL, NULL};
    }
    *err = burrow_err_out_of_memory;
    return (Any){NULL, NULL};
}

static Error lite_rows_next(void *self, Slice dest) {
    LiteRows *r = self;
    sqlite3_stmt *s = r->st->s;
    if (s == NULL)
        return io_eof;
    int rc = sqlite3_step(s);
    if (rc == SQLITE_DONE)
        return io_eof;
    if (rc != SQLITE_ROW)
        return lite_error(r->st->c->db);
    arena_reset(&r->row);
    Alloc *a = arena_allocator(&r->row);
    Any *d = dest.p;
    Error err = BURROW_NO_ERROR;
    for (Int i = 0; i < r->ncols && BURROW_OK(err); i++)
        d[i] = lite_column(r, a, (int)i, &err);
    return err;
}

static Str lite_rows_column_type_database_type_name(void *self, Int index) {
    return ((LiteRows *)self)->decl[index];
}

static const DriverRowsVT lite_rows_vt = {
    .columns = lite_rows_columns,
    .close = lite_rows_close,
    .next = lite_rows_next,
    .column_type_database_type_name = lite_rows_column_type_database_type_name,
};

/* The declared type of a column the way database/sql wants it, upper case and
 * without a length: "VARCHAR(20)" is "VARCHAR". "" for an expression. */
static Str lite_decl_type(Alloc *a, const char *decl) {
    if (decl == NULL)
        return BURROW_STR_EMPTY;
    Str d = str_from_cstr(decl);
    Int paren = strings_index_byte(d, '(');
    if (paren >= 0)
        d.len = paren;
    return strings_to_upper(a, strings_trim_space(d));
}

static DriverRows lite_rows_new(LiteStmt *st, Error *err) {
    LiteRows *r = BURROW_NEW(heap_allocator(), LiteRows);
    if (r == NULL) {
        *err = burrow_err_out_of_memory;
        return (DriverRows){NULL, NULL};
    }
    r->st = st;
    arena_init(&r->names, heap_allocator(), 0);
    arena_init(&r->row, heap_allocator(), 0);
    Alloc *a = arena_allocator(&r->names);
    r->ncols = st->s == NULL ? 0 : (Int)sqlite3_column_count(st->s);
    Str *cols = BURROW_NEW_N(a, Str, (size_t)r->ncols + 1);
    r->decl = BURROW_NEW_N(a, Str, (size_t)r->ncols + 1);
    if (cols == NULL || r->decl == NULL) {
        arena_free(&r->row);
        arena_free(&r->names);
        mem_free(heap_allocator(), r, sizeof *r, _Alignof(LiteRows));
        *err = burrow_err_out_of_memory;
        return (DriverRows){NULL, NULL};
    }
    for (Int i = 0; i < r->ncols; i++) {
        cols[i] = str_clone(a, str_from_cstr(sqlite3_column_name(st->s, (int)i)));
        r->decl[i] = lite_decl_type(a, sqlite3_column_decltype(st->s, (int)i));
    }
    r->cols = slice_from(cols, r->ncols, r->ncols, TYPE_STRING);
    st->rows_open = true;
    return (DriverRows){&lite_rows_vt, r};
}

/* -------------------------------------------------------------------- Stmt */

static Error lite_stmt_close(void *self) {
    LiteStmt *st = self;
    if (st->rows_open) {
        st->closed = true; /* the Rows finalizes it */
        return BURROW_NO_ERROR;
    }
    (void)sqlite3_finalize(st->s);
    mem_free(heap_allocator(), st, sizeof *st, _Alignof(LiteStmt));
    return BURROW_NO_ERROR;
}

static Int lite_stmt_num_input(void *self) {
    LiteStmt *st = self;
    return st->s == NULL ? 0 : (Int)sqlite3_bind_parameter_count(st->s);
}

static Error lite_stmt_start(LiteStmt *st, Context ctx, Slice args) {
    Error err = context_err(ctx);
    if (BURROW_FAILED(err))
        return err;
    if (st->rows_open)
        return fmt_errorf_v("sqlite: the statement's rows are still open");
    if (st->s == NULL)
        return BURROW_NO_ERROR;
    return lite_bind(st, args);
}

static DriverResult lite_stmt_exec_context(void *self, Alloc *a, Context ctx,
                                           Slice args, Error *err) {
    LiteStmt *st = self;
    *err = lite_stmt_start(st, ctx, args);
    if (BURROW_FAILED(*err))
        return (DriverResult){NULL, NULL};
    if (st->s != NULL) {
        int rc;
        while ((rc = sqlite3_step(st->s)) == SQLITE_ROW) {
        }
        if (rc != SQLITE_DONE) {
            *err = lite_error(st->c->db);
            (void)sqlite3_reset(st->s);
            return (DriverResult){NULL, NULL};
        }
        (void)sqlite3_reset(st->s);
    }
    return lite_result(a, st->c->db, err);
}

static DriverRows lite_stmt_query_context(void *self, Context ctx, Slice args,
                                          Error *err) {
    LiteStmt *st = self;
    *err = lite_stmt_start(st, ctx, args);
    if (BURROW_FAILED(*err))
        return (DriverRows){NULL, NULL};
    return lite_rows_new(st, err);
}

static const DriverStmtVT lite_stmt_vt = {
    .close = lite_stmt_close,
    .num_input = lite_stmt_num_input,
    .exec_context = lite_stmt_exec_context,
    .query_context = lite_stmt_query_context,
};

/* -------------------------------------------------------------------- Conn */

/* Whether there is nothing but spaces and semicolons after a statement. */
static bool lite_only_space(const char *tail, const char *end) {
    for (; tail < end; tail++)
        if (*tail != ';' && *tail != ' ' && *tail != '\t' && *tail != '\n' &&
            *tail != '\r')
            return false;
    return true;
}

static DriverStmt lite_conn_prepare_context(void *self, Context ctx, Str query,
                                            Error *err) {
    LiteConn *c = self;
    *err = context_err(ctx);
    if (BURROW_FAILED(*err))
        return (DriverStmt){NULL, NULL};
    sqlite3_stmt *s = NULL;
    const char *tail = NULL;
    const char *q = (const char *)query.p;
    if (sqlite3_prepare_v2(c->db, q, (int)query.len, &s, &tail) != SQLITE_OK) {
        *err = lite_error(c->db);
        return (DriverStmt){NULL, NULL};
    }
    if (tail != NULL && !lite_only_space(tail, q + query.len)) {
        (void)sqlite3_finalize(s);
        *err = fmt_errorf_v("sqlite: a prepared statement can only be one statement");
        return (DriverStmt){NULL, NULL};
    }
    LiteStmt *st = BURROW_NEW(heap_allocator(), LiteStmt);
    if (st == NULL) {
        (void)sqlite3_finalize(s);
        *err = burrow_err_out_of_memory;
        return (DriverStmt){NULL, NULL};
    }
    st->c = c;
    st->s = s;
    return (DriverStmt){&lite_stmt_vt, st};
}

static DriverStmt lite_conn_prepare(void *self, Str query, Error *err) {
    return lite_conn_prepare_context(self, context_background(), query, err);
}

/* Runs a script of statements without arguments, the way sqlite3_exec does,
 * which is what a schema wants. With arguments it hands back to database/sql,
 * which prepares the query, and then it has to be one statement. */
static DriverResult lite_conn_exec_context(void *self, Alloc *a, Context ctx, Str query,
                                           Slice args, Error *err) {
    LiteConn *c = self;
    if (args.len > 0) {
        *err = driver_err_skip;
        return (DriverResult){NULL, NULL};
    }
    *err = context_err(ctx);
    if (BURROW_FAILED(*err))
        return (DriverResult){NULL, NULL};
    const char *q = (const char *)query.p;
    const char *end = q + query.len;
    while (q < end) {
        sqlite3_stmt *s = NULL;
        const char *tail = NULL;
        if (sqlite3_prepare_v2(c->db, q, (int)(end - q), &s, &tail) != SQLITE_OK) {
            *err = lite_error(c->db);
            return (DriverResult){NULL, NULL};
        }
        if (s != NULL) {
            int rc;
            while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
            }
            (void)sqlite3_finalize(s);
            if (rc != SQLITE_DONE) {
                *err = lite_error(c->db);
                return (DriverResult){NULL, NULL};
            }
        }
        if (tail == NULL || tail <= q)
            break;
        q = tail;
    }
    return lite_result(a, c->db, err);
}

static Error lite_conn_exec_plain(LiteConn *c, const char *sql) {
    if (sqlite3_exec(c->db, sql, NULL, NULL, NULL) != SQLITE_OK)
        return lite_error(c->db);
    return BURROW_NO_ERROR;
}

/* Ends a transaction with COMMIT or ROLLBACK, and turns query_only off again
 * after a read only one. */
static Error lite_tx_end(LiteConn *c, const char *sql) {
    Error err = lite_conn_exec_plain(c, sql);
    if (c->read_only_tx) {
        c->read_only_tx = false;
        Error e = lite_conn_exec_plain(c, "PRAGMA query_only = OFF");
        if (BURROW_OK(err))
            err = e;
    }
    return err;
}

static Error lite_tx_commit(void *self) {
    return lite_tx_end(self, "COMMIT");
}

static Error lite_tx_rollback(void *self) {
    return lite_tx_end(self, "ROLLBACK");
}

static const DriverTxVT lite_tx_vt = {
    .commit = lite_tx_commit,
    .rollback = lite_tx_rollback,
};

/* SQLite's transactions are serializable, which satisfies every level up to
 * that one. A read only transaction is made one with the query_only pragma. */
static DriverTx lite_conn_begin_tx(void *self, Context ctx, DriverTxOptions opts,
                                   Error *err) {
    LiteConn *c = self;
    *err = context_err(ctx);
    if (BURROW_FAILED(*err))
        return (DriverTx){NULL, NULL};
    if (opts.isolation > SQL_LEVEL_SERIALIZABLE) {
        *err = fmt_errorf_v("sqlite: isolation level %d is not supported",
                            (Int)opts.isolation);
        return (DriverTx){NULL, NULL};
    }
    if (opts.read_only) {
        *err = lite_conn_exec_plain(c, "PRAGMA query_only = ON");
        if (BURROW_FAILED(*err))
            return (DriverTx){NULL, NULL};
        c->read_only_tx = true;
    }
    *err = lite_conn_exec_plain(c, "BEGIN");
    if (BURROW_FAILED(*err)) {
        if (c->read_only_tx) {
            c->read_only_tx = false;
            (void)lite_conn_exec_plain(c, "PRAGMA query_only = OFF");
        }
        return (DriverTx){NULL, NULL};
    }
    return (DriverTx){&lite_tx_vt, c};
}

static DriverTx lite_conn_begin(void *self, Error *err) {
    return lite_conn_begin_tx(self, context_background(), (DriverTxOptions){0}, err);
}

static Error lite_conn_ping(void *self, Context ctx) {
    LiteConn *c = self;
    Error err = context_err(ctx);
    if (BURROW_FAILED(err))
        return err;
    return c->db == NULL ? driver_err_bad_conn : BURROW_NO_ERROR;
}

static Error lite_conn_close(void *self) {
    LiteConn *c = self;
    int rc = sqlite3_close_v2(c->db);
    Error err = BURROW_NO_ERROR;
    if (rc != SQLITE_OK)
        err = lite_error(c->db);
    mem_free(heap_allocator(), c, sizeof *c, _Alignof(LiteConn));
    return err;
}

static const DriverConnVT lite_conn_vt = {
    .prepare = lite_conn_prepare,
    .close = lite_conn_close,
    .begin = lite_conn_begin,
    .prepare_context = lite_conn_prepare_context,
    .begin_tx = lite_conn_begin_tx,
    .ping = lite_conn_ping,
    .exec_context = lite_conn_exec_context,
};

/* ------------------------------------------------------------------ Driver */

static DriverConn lite_open(void *self, Str name, Error *err) {
    (void)self;
    char *path = str_to_cstr(heap_allocator(), name);
    if (path == NULL) {
        *err = burrow_err_out_of_memory;
        return (DriverConn){NULL, NULL};
    }
    sqlite3 *db = NULL;
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI |
                SQLITE_OPEN_FULLMUTEX;
    int rc = sqlite3_open_v2(path, &db, flags, NULL);
    mem_free(heap_allocator(), path, (size_t)name.len + 1, 1);
    if (rc != SQLITE_OK) {
        *err = db != NULL ? lite_error(db) : burrow_err_out_of_memory;
        (void)sqlite3_close_v2(db);
        return (DriverConn){NULL, NULL};
    }
    (void)sqlite3_extended_result_codes(db, 1);
    (void)sqlite3_busy_timeout(db, LITE_BUSY_TIMEOUT_MS);
    LiteConn *c = BURROW_NEW(heap_allocator(), LiteConn);
    if (c == NULL) {
        (void)sqlite3_close_v2(db);
        *err = burrow_err_out_of_memory;
        return (DriverConn){NULL, NULL};
    }
    c->db = db;
    *err = BURROW_NO_ERROR;
    return (DriverConn){&lite_conn_vt, c};
}

static const DriverVT lite_driver_vt = {.open = lite_open};

const Driver sqlite_driver = {&lite_driver_vt, NULL};

void sqlite_register(void) {
    sql_register(BURROW_S("sqlite"), sqlite_driver);
}
