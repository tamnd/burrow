/* Go's fakedb_test.go: a fake driver for the database/sql tests, and the
 * basic and scancols variants of it from sql_test.go.
 *
 * It speaks a query language that's semantically similar to but syntactically
 * different and simpler than SQL. The syntax is as follows:
 *
 *     WIPE
 *     CREATE|<tablename>|<col>=<type>,<col>=<type>,...
 *       where types are: "string", [u]int{8,16,32,64}, "bool"
 *     INSERT|<tablename>|col=val,col2=val2,col3=?
 *     SELECT|<tablename>|projectcol1,projectcol2|filtercol=?,filtercol2=?
 *     SELECT|<tablename>|projectcol1,projectcol2|filtercol=?param1,filtercol2=?param2
 *
 * Any of these can be preceded by PANIC|<method>|, to cause the named method
 * on the statement to panic, or by WAIT|<duration>|, to make it sleep for that
 * long first. Several can be joined with a semicolon.
 *
 * Opening a database starts it empty, with no tables, and everything is kept
 * in memory. What a table holds comes from one arena that lives as long as the
 * program, since Go's collector would keep a row alive for as long as anything
 * can still see it.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef DATABASE_SQL_FAKEDB_H
#define DATABASE_SQL_FAKEDB_H

#include "burrow/bytes.h"
#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/database/sql.h"
#include "burrow/database/sql/driver.h"
#include "burrow/declare.h"
#include "burrow/defer.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/time.h"
#include "check.h"

#include "../src/database/sql_internal.h"

#include <string.h>

/* ------------------------------------------------------------ memory */

static SyncMutex fdb_keep_mu;
static Arena fdb_keep_arena;
static bool fdb_keep_ready;

/* Memory that lasts as long as the program. */
static void *fdb_keep(size_t n) {
    sync_mutex_lock(&fdb_keep_mu);
    if (!fdb_keep_ready) {
        arena_init(&fdb_keep_arena, heap_allocator(), 0);
        fdb_keep_ready = true;
    }
    void *p = mem_alloc(arena_allocator(&fdb_keep_arena), n == 0 ? 1 : n, 16);
    sync_mutex_unlock(&fdb_keep_mu);
    if (p == NULL)
        panic_str(BURROW_S("fakedb: out of memory"));
    return p;
}

static Str fdb_keep_str(Str s) {
    if (s.len == 0)
        return BURROW_STR_EMPTY;
    Byte *p = fdb_keep((size_t)s.len);
    memcpy(p, s.p, (size_t)s.len);
    return (Str){p, s.len};
}

static bool fdb_is_bytes(const Type *t) {
    return t != NULL && t->kind == KIND_SLICE && t->elem == TYPE_BYTE;
}

/* A copy of a driver value that lasts, deep enough that nothing in it points
 * at the caller's memory. */
static Any fdb_keep_value(Any v) {
    if (v.t == NULL)
        return v;
    if (v.t == TYPE_STRING) {
        Str *s = fdb_keep(sizeof(Str));
        *s = fdb_keep_str(*(const Str *)v.data);
        return (Any){TYPE_STRING, s};
    }
    if (fdb_is_bytes(v.t)) {
        const Bytes *b = v.data;
        Bytes *out = fdb_keep(sizeof(Bytes));
        *out = (Bytes){NULL, 0, 0, TYPE_BYTE};
        if (b->p != NULL) {
            Byte *p = fdb_keep((size_t)b->len);
            if (b->len > 0)
                memcpy(p, b->p, (size_t)b->len);
            *out = slice_from(p, b->len, b->len, TYPE_BYTE);
        }
        return (Any){v.t, out};
    }
    void *p = fdb_keep(v.t->size);
    memcpy(p, v.data, v.t->size);
    return (Any){v.t, p};
}

static bool fdb_str_is(Any v, const char *s) {
    return v.t == TYPE_STRING && str_eq(*(const Str *)v.data, str_from_cstr(s));
}

/* Go's fakeError, an error with a message of its own that wraps another. */
typedef struct FakeError {
    Str message;
    Error wrapped;
} FakeError;

static Str fdb_error_message(const void *self) {
    return ((const FakeError *)self)->message;
}

static Error fdb_error_unwrap(const void *self) {
    return ((const FakeError *)self)->wrapped;
}

static const ErrorVT fdb_error_vt = {
    NULL, fdb_error_message, fdb_error_unwrap, NULL, NULL, NULL, NULL};

static Error fdb_error(const char *message, Error wrapped) {
    FakeError *e = BURROW_NEW(error_allocator(), FakeError);
    if (e == NULL)
        return wrapped;
    e->message = str_from_cstr(message);
    e->wrapped = wrapped;
    return (Error){&fdb_error_vt, e};
}

/* ------------------------------------------------------- the database */

typedef struct FakeTable {
    struct FakeTable *next;
    Str name;
    SyncMutex mu;
    Int ncols;
    Str *colname;
    Str *coltype;
    Any **rows;
    Int nrows;
    Int caprows;
} FakeTable;

typedef struct FakeDB {
    struct FakeDB *next;
    Str name;
    SyncMutex mu;
    FakeTable *tables;
    bool bad_conn;
    bool allow_any;
} FakeDB;

static Int fdb_column_index(const FakeTable *t, Str name) {
    for (Int i = 0; i < t->ncols; i++) {
        if (str_eq(t->colname[i], name))
            return i;
    }
    return -1;
}

static SyncMutex fdb_mu; /* guards the following */
static Int fdb_open_count;
static Int fdb_close_count;
static Chan *fdb_wait_ch;
static Chan *fdb_waiting_ch;
static FakeDB *fdb_dbs;

static FakeDB *fdb_get_db(Str name) {
    sync_mutex_lock(&fdb_mu);
    FakeDB *db = fdb_dbs;
    while (db != NULL && !str_eq(db->name, name))
        db = db->next;
    if (db == NULL) {
        db = fdb_keep(sizeof(FakeDB));
        db->name = fdb_keep_str(name);
        db->next = fdb_dbs;
        fdb_dbs = db;
    }
    sync_mutex_unlock(&fdb_mu);
    return db;
}

static void fdb_wipe(FakeDB *db) {
    sync_mutex_lock(&db->mu);
    db->tables = NULL;
    sync_mutex_unlock(&db->mu);
}

/* Must be called with db->mu held. */
static FakeTable *fdb_table(FakeDB *db, Str name) {
    FakeTable *t = db->tables;
    while (t != NULL && !str_eq(t->name, name))
        t = t->next;
    return t;
}

static Error fdb_create_table(FakeDB *db, Str name, Int ncols, const Str *names,
                              const Str *types) {
    sync_mutex_lock(&db->mu);
    if (fdb_table(db, name) != NULL) {
        sync_mutex_unlock(&db->mu);
        return fmt_errorf_v("fakedb: table %q already exists", name);
    }
    FakeTable *t = fdb_keep(sizeof(FakeTable));
    t->name = fdb_keep_str(name);
    t->ncols = ncols;
    t->colname = fdb_keep((size_t)ncols * sizeof(Str));
    t->coltype = fdb_keep((size_t)ncols * sizeof(Str));
    for (Int i = 0; i < ncols; i++) {
        t->colname[i] = fdb_keep_str(names[i]);
        t->coltype[i] = fdb_keep_str(types[i]);
    }
    t->next = db->tables;
    db->tables = t;
    sync_mutex_unlock(&db->mu);
    return BURROW_NO_ERROR;
}

static bool fdb_column_type(FakeDB *db, Str table, Str column, Str *typ) {
    sync_mutex_lock(&db->mu);
    FakeTable *t = fdb_table(db, table);
    bool ok = false;
    if (t != NULL) {
        Int i = fdb_column_index(t, column);
        if (i != -1) {
            *typ = t->coltype[i];
            ok = true;
        }
    }
    sync_mutex_unlock(&db->mu);
    return ok;
}

/* -------------------------------------------------------------- hooks */

/* hookOpenErr, to simulate connection failures. */
static SyncMutex fdb_hook_open_err_mu;
static Error (*fdb_hook_open_err)(void);

BURROW_UNUSED static void fdb_set_hook_open_err(Error (*fn)(void)) {
    sync_mutex_lock(&fdb_hook_open_err_mu);
    fdb_hook_open_err = fn;
    sync_mutex_unlock(&fdb_hook_open_err_mu);
}

/* Hooks to simulate broken connections. */
static bool (*fdb_hook_prepare_bad_conn)(void);
static bool (*fdb_hook_exec_bad_conn)(void);
static bool (*fdb_hook_query_bad_conn)(void);
static bool (*fdb_hook_commit_bad_conn)(void);
static bool (*fdb_hook_rollback_bad_conn)(void);

typedef struct FakeConn FakeConn;

static SyncMutex fdb_hook_post_close_conn_mu;
static void (*fdb_hook_post_close_conn)(FakeConn *c, Error err);

static void fdb_set_hook_post_close_conn(void (*fn)(FakeConn *, Error)) {
    sync_mutex_lock(&fdb_hook_post_close_conn_mu);
    fdb_hook_post_close_conn = fn;
    sync_mutex_unlock(&fdb_hook_post_close_conn_mu);
}

/* setStrictFakeConnClose: the test to fail when a conn fails to close, or
 * NULL for none. */
static TestingT *fdb_strict_close;

static Error (*fdb_rows_cursor_next_hook)(Slice dest);

/* --------------------------------------------------------------- conn */

typedef void (*FakeWaiter)(Context ctx);

typedef struct FakeTx {
    FakeConn *c;
} FakeTx;

struct FakeConn {
    FakeDB *db; /* where to return ourselves to */

    FakeTx *curr_tx;
    FakeTx tx;

    /* Every operation writes to line, so that the thread sanitizer sees a
     * race if two of them run at once. */
    int64_t line;

    /* Stats for tests. */
    SyncMutex mu;
    Int stmts_made;
    Int stmts_closed;
    Int num_prepare;

    /* bad connection tests; see fdb_conn_is_bad */
    bool bad;
    bool sticky_bad;

    bool skip_dirty_session; /* tests that use Conn should set this to true */

    /* dirty_session tests ResetSession, true if a query has executed until
     * ResetSession is called. */
    bool dirty_session;

    /* The waiter is called before each query. May be used in place of the
     * WAIT directive. */
    FakeWaiter waiter;
};

static void fdb_touch(FakeConn *c) {
    c->line++;
}

static void fdb_incr_stat(FakeConn *c, Int *v) {
    sync_mutex_lock(&c->mu);
    (*v)++;
    sync_mutex_unlock(&c->mu);
}

static bool fdb_conn_is_bad(FakeConn *c) {
    if (c->sticky_bad)
        return true;
    if (c->bad) {
        if (c->db == NULL)
            return false;
        /* alternate between bad conn and not bad conn */
        c->db->bad_conn = !c->db->bad_conn;
        return c->db->bad_conn;
    }
    return false;
}

static bool fdb_conn_is_dirty_and_mark(FakeConn *c) {
    if (c->skip_dirty_session)
        return false;
    if (c->curr_tx != NULL) {
        c->dirty_session = true;
        return false;
    }
    if (c->dirty_session)
        return true;
    c->dirty_session = true;
    return false;
}

static Error fdb_tx_commit(void *self) {
    FakeTx *tx = self;
    tx->c->curr_tx = NULL;
    if (fdb_hook_commit_bad_conn != NULL && fdb_hook_commit_bad_conn())
        return fdb_error("Commit: Hook Bad Conn", driver_err_bad_conn);
    fdb_touch(tx->c);
    return BURROW_NO_ERROR;
}

static Error fdb_tx_rollback(void *self) {
    FakeTx *tx = self;
    tx->c->curr_tx = NULL;
    if (fdb_hook_rollback_bad_conn != NULL && fdb_hook_rollback_bad_conn())
        return fdb_error("Rollback: Hook Bad Conn", driver_err_bad_conn);
    fdb_touch(tx->c);
    return BURROW_NO_ERROR;
}

static const DriverTxVT fdb_tx_vt = {NULL, fdb_tx_commit, fdb_tx_rollback};

static DriverTx fdb_conn_begin(void *self, Error *err) {
    FakeConn *c = self;
    if (fdb_conn_is_bad(c)) {
        *err = fdb_error("", driver_err_bad_conn);
        return (DriverTx){NULL, NULL};
    }
    if (c->curr_tx != NULL) {
        *err = fmt_errorf_v("fakedb: already in a transaction");
        return (DriverTx){NULL, NULL};
    }
    fdb_touch(c);
    c->tx.c = c;
    c->curr_tx = &c->tx;
    *err = BURROW_NO_ERROR;
    return (DriverTx){&fdb_tx_vt, c->curr_tx};
}

static Error fdb_conn_reset_session(void *self, Context ctx) {
    (void)ctx;
    FakeConn *c = self;
    c->dirty_session = false;
    c->curr_tx = NULL;
    if (fdb_conn_is_bad(c))
        return fdb_error("Reset Session: bad conn", driver_err_bad_conn);
    return BURROW_NO_ERROR;
}

static bool fdb_conn_is_valid(void *self) {
    return !fdb_conn_is_bad(self);
}

static Error fdb_conn_close_inner(FakeConn *c) {
    fdb_touch(c);
    if (c->curr_tx != NULL)
        return fmt_errorf_v("fakedb: can't close fakeConn; in a Transaction");
    if (c->db == NULL)
        return fmt_errorf_v("fakedb: can't close fakeConn; already closed");
    if (c->stmts_made > c->stmts_closed)
        return fmt_errorf_v("fakedb: can't close; dangling statement(s)");
    c->db = NULL;
    return BURROW_NO_ERROR;
}

/* Close is the end of the conn either way, since database/sql forgets it
 * whatever close says. */
static Error fdb_conn_close(void *self) {
    FakeConn *c = self;
    Error err = fdb_conn_close_inner(c);
    if (BURROW_FAILED(err) && fdb_strict_close != NULL)
        testing_t_errorf_v(fdb_strict_close, "failed to close a test fakeConn: %v",
                           err);
    sync_mutex_lock(&fdb_hook_post_close_conn_mu);
    void (*fn)(FakeConn *, Error) = fdb_hook_post_close_conn;
    sync_mutex_unlock(&fdb_hook_post_close_conn_mu);
    if (fn != NULL)
        fn(c, err);
    if (!BURROW_FAILED(err)) {
        sync_mutex_lock(&fdb_mu);
        fdb_close_count++;
        sync_mutex_unlock(&fdb_mu);
    }
    mem_free(heap_allocator(), c, sizeof(FakeConn), _Alignof(FakeConn));
    return err;
}

static Error fdb_check_subset_types(bool allow_any, Slice args) {
    const DriverNamedValue *nv = args.p;
    for (Int i = 0; i < args.len; i++) {
        const Type *t = nv[i].value.t;
        if (t == NULL || t == TYPE_INT64 || t == TYPE_FLOAT64 || t == TYPE_BOOL ||
            t == TYPE_BYTES || t == TYPE_STRING || t == TYPE_TIME)
            continue;
        if (!allow_any)
            return fmt_errorf_v(
                "fakedb: invalid argument ordinal %[1]d: %[2]v, type %[2]T",
                nv[i].ordinal, nv[i].value);
    }
    return BURROW_NO_ERROR;
}

/* This is an optional slot, but it's set here just to check that all the args
 * are of the proper types. driver_err_skip is returned so the caller acts as if
 * we didn't set it at all. */
static DriverResult fdb_conn_exec_context(void *self, Alloc *a, Context ctx, Str query,
                                          Slice args, Error *err) {
    (void)a;
    (void)ctx;
    (void)query;
    FakeConn *c = self;
    *err = fdb_check_subset_types(c->db->allow_any, args);
    if (!BURROW_FAILED(*err))
        *err = driver_err_skip;
    return (DriverResult){NULL, NULL};
}

static DriverRows fdb_conn_query_context(void *self, Context ctx, Str query, Slice args,
                                         Error *err) {
    (void)ctx;
    (void)query;
    FakeConn *c = self;
    *err = fdb_check_subset_types(c->db->allow_any, args);
    if (!BURROW_FAILED(*err))
        *err = driver_err_skip;
    return (DriverRows){NULL, NULL};
}

/* --------------------------------------------------------------- stmt */

typedef struct FakeBoundCol {
    Str column;
    Str placeholder;
    Int ordinal;
} FakeBoundCol;

typedef struct FakeStmt {
    FakeConn *c;
    Arena arena;
    Str q; /* just for debugging */

    Str cmd;
    Str table;
    Str panic;
    Duration wait;

    struct FakeStmt *next; /* used for returning multiple results */

    bool closed;

    Int ncols;
    Str *col_name;    /* used by CREATE, INSERT, SELECT (selected columns) */
    Str *col_type;    /* used by CREATE */
    Any *col_value;   /* used by INSERT (mix of strings and "?" for bound params) */
    Int placeholders; /* used by INSERT/SELECT: number of ? params */

    FakeBoundCol *where_col; /* used by SELECT (all placeholders) */
    Int nwhere;

    DriverValueConverter *placeholder_converter; /* used by INSERT */
    Int nconv;
} FakeStmt;

static Alloc *fdb_stmt_alloc(FakeStmt *s) {
    return arena_allocator(&s->arena);
}

static FakeStmt *fdb_stmt_new(FakeConn *c, Str q) {
    FakeStmt *s = BURROW_NEW(heap_allocator(), FakeStmt);
    if (s == NULL)
        panic_str(BURROW_S("fakedb: out of memory"));
    s->c = c;
    arena_init(&s->arena, heap_allocator(), 0);
    s->q = q.len > 0 ? str_clone(fdb_stmt_alloc(s), q) : BURROW_STR_EMPTY;
    return s;
}

/* Frees s and the statements after it. */
static void fdb_stmt_free(FakeStmt *s) {
    while (s != NULL) {
        FakeStmt *next = s->next;
        arena_free(&s->arena);
        mem_free(heap_allocator(), s, sizeof(FakeStmt), _Alignof(FakeStmt));
        s = next;
    }
}

/* Go's fakeStmt.Close, without the freeing. */
static Error fdb_stmt_close_go(FakeStmt *s) {
    if (str_eq(s->panic, BURROW_S("Close")))
        panic_str(s->panic);
    if (s->c == NULL)
        panic_str(BURROW_S("nil conn in fakeStmt.Close"));
    if (s->c->db == NULL)
        panic_str(BURROW_S("in fakeStmt.Close, conn's db is nil (already closed)"));
    fdb_touch(s->c);
    if (!s->closed) {
        fdb_incr_stat(s->c, &s->c->stmts_closed);
        s->closed = true;
    }
    if (s->next != NULL)
        (void)fdb_stmt_close_go(s->next);
    return BURROW_NO_ERROR;
}

static void fdb_stmt_free_fn(void *s) {
    fdb_stmt_free((FakeStmt *)s);
}

/* The statement is freed even when PANIC|Close| makes the close panic. */
static Error fdb_stmt_close(void *self) {
    FakeStmt *s = self;
    Error e = BURROW_NO_ERROR;
    BURROW_SCOPE {
        BURROW_DEFER(fdb_stmt_free_fn, s);
        e = fdb_stmt_close_go(s);
    }
    BURROW_SCOPE_END;
    return e;
}

static Slice fdb_split(FakeStmt *s, Str str, const char *sep) {
    Slice parts = strings_split(fdb_stmt_alloc(s), str, str_from_cstr(sep));
    if (parts.p == NULL && str.len > 0)
        panic_str(BURROW_S("fakedb: out of memory"));
    return parts;
}

static Str fdb_part(Slice parts, Int i) {
    return ((const Str *)parts.p)[i];
}

/* parts are table|selectCol1,selectCol2|whereCol=?,whereCol2=? (note that
 * where columns must always contain ? marks, just a limitation for fakedb) */
static Error fdb_prepare_select(FakeConn *c, FakeStmt *s, Slice parts) {
    if (parts.len != 3) {
        (void)fdb_stmt_close_go(s);
        return fmt_errorf_v("fakedb: invalid SELECT syntax with %d parts; want 3",
                            parts.len);
    }
    s->table = fdb_part(parts, 0);
    Slice cols = fdb_split(s, fdb_part(parts, 1), ",");
    s->col_name = (Str *)cols.p;
    s->ncols = cols.len;
    Slice specs = fdb_split(s, fdb_part(parts, 2), ",");
    s->where_col = BURROW_NEW_N(fdb_stmt_alloc(s), FakeBoundCol, (size_t)specs.len + 1);
    for (Int n = 0; n < specs.len; n++) {
        Str colspec = fdb_part(specs, n);
        if (colspec.len == 0)
            continue;
        Slice name_val = fdb_split(s, colspec, "=");
        if (name_val.len != 2) {
            (void)fdb_stmt_close_go(s);
            return fmt_errorf_v(
                "fakedb: SELECT on table %q has invalid column spec of %q (index %d)",
                s->table, colspec, n);
        }
        Str column = fdb_part(name_val, 0), value = fdb_part(name_val, 1);
        Str typ = BURROW_STR_EMPTY;
        if (!fdb_column_type(c->db, s->table, column, &typ)) {
            (void)fdb_stmt_close_go(s);
            return fmt_errorf_v(
                "fakedb: SELECT on table %q references non-existent column %q",
                s->table, column);
        }
        if (!strings_has_prefix(value, BURROW_S("?"))) {
            (void)fdb_stmt_close_go(s);
            return fmt_errorf_v("fakedb: SELECT on table %q has pre-bound value for "
                                "where column %q; need a question mark",
                                s->table, column);
        }
        s->placeholders++;
        s->where_col[s->nwhere++] = (FakeBoundCol){column, value, s->placeholders};
    }
    return BURROW_NO_ERROR;
}

/* parts are table|col=type,col2=type2 */
static Error fdb_prepare_create(FakeStmt *s, Slice parts) {
    if (parts.len != 2) {
        (void)fdb_stmt_close_go(s);
        return fmt_errorf_v("fakedb: invalid CREATE syntax with %d parts; want 2",
                            parts.len);
    }
    s->table = fdb_part(parts, 0);
    Slice specs = fdb_split(s, fdb_part(parts, 1), ",");
    s->col_name = BURROW_NEW_N(fdb_stmt_alloc(s), Str, (size_t)specs.len);
    s->col_type = BURROW_NEW_N(fdb_stmt_alloc(s), Str, (size_t)specs.len);
    for (Int n = 0; n < specs.len; n++) {
        Str colspec = fdb_part(specs, n);
        Slice name_type = fdb_split(s, colspec, "=");
        if (name_type.len != 2) {
            (void)fdb_stmt_close_go(s);
            return fmt_errorf_v(
                "fakedb: CREATE table %q has invalid column spec of %q (index %d)",
                s->table, colspec, n);
        }
        s->col_name[s->ncols] = fdb_part(name_type, 0);
        s->col_type[s->ncols] = fdb_part(name_type, 1);
        s->ncols++;
    }
    return BURROW_NO_ERROR;
}

/* fakeDriverString is like driver_string, but follows pointers like the
 * default converter does. */
static DriverValue fdb_string_convert(void *self, Alloc *a, Any v, Error *err) {
    (void)self;
    *err = BURROW_NO_ERROR;
    if (v.t == TYPE_STRING || fdb_is_bytes(v.t))
        return v;
    if (v.t != NULL && v.t->kind == KIND_POINTER && v.t->elem == TYPE_STRING) {
        const Str *p = *(Str *const *)v.data;
        if (p == NULL)
            return (Any){NULL, NULL};
        return v.t->elem == TYPE_STRING ? (Any){TYPE_STRING, (void *)(uintptr_t)p} : v;
    }
    Str *s = BURROW_NEW(a, Str);
    if (s == NULL) {
        *err = burrow_err_out_of_memory;
        return (Any){NULL, NULL};
    }
    *s = fmt_sprintf_v(a, "%v", v);
    return (Any){TYPE_STRING, s};
}

static const DriverValueConverterVT fdb_string_vt = {NULL, fdb_string_convert};
static const DriverValueConverter fdb_driver_string = {&fdb_string_vt, NULL};

static DriverValue fdb_any_convert(void *self, Alloc *a, Any v, Error *err) {
    (void)self;
    (void)a;
    *err = BURROW_NO_ERROR;
    return v;
}

static const DriverValueConverterVT fdb_any_vt = {NULL, fdb_any_convert};
static const DriverValueConverter fdb_any_type_converter = {&fdb_any_vt, NULL};

static DriverNull fdb_null_bool, fdb_null_default, fdb_null_string;
static DriverNotNull fdb_not_null_default, fdb_not_null_string;

static DriverValueConverter fdb_converter_for_type(Str typ) {
    fdb_null_bool.converter = driver_bool;
    fdb_null_default.converter = driver_default_parameter_converter;
    fdb_null_string.converter = fdb_driver_string;
    fdb_not_null_default.converter = driver_default_parameter_converter;
    fdb_not_null_string.converter = fdb_driver_string;
    static const char *const not_null_default[] = {"byte",    "int16",    "int64",
                                                   "float64", "datetime", "uuid"};
    static const char *const null_default[] = {
        "nullbyte",    "nullint32",    "nullint16", "nullint64",
        "nullfloat64", "nulldatetime", "nulluuid",  "nulltable"};
    if (str_eq(typ, BURROW_S("bool")))
        return driver_bool;
    if (str_eq(typ, BURROW_S("nullbool")))
        return driver_null_as_value_converter(&fdb_null_bool);
    if (str_eq(typ, BURROW_S("int32")))
        return driver_int32;
    if (str_eq(typ, BURROW_S("string")))
        return driver_not_null_as_value_converter(&fdb_not_null_string);
    if (str_eq(typ, BURROW_S("nullstring")))
        return driver_null_as_value_converter(&fdb_null_string);
    if (str_eq(typ, BURROW_S("any")))
        return fdb_any_type_converter;
    for (size_t i = 0; i < sizeof not_null_default / sizeof *not_null_default; i++) {
        if (str_eq(typ, str_from_cstr(not_null_default[i])))
            return driver_not_null_as_value_converter(&fdb_not_null_default);
    }
    for (size_t i = 0; i < sizeof null_default / sizeof *null_default; i++) {
        if (str_eq(typ, str_from_cstr(null_default[i])))
            return driver_null_as_value_converter(&fdb_null_default);
    }
    panic_str(
        fmt_sprintf_v(error_allocator(), "invalid fakedb column type of %s", typ));
}

static const Type *fdb_col_type_to_type(Str typ) {
    if (str_eq(typ, BURROW_S("bool")))
        return TYPE_BOOL;
    if (str_eq(typ, BURROW_S("nullbool")))
        return &burrow_type_SqlNullBool;
    if (str_eq(typ, BURROW_S("int16")))
        return TYPE_INT16;
    if (str_eq(typ, BURROW_S("nullint16")))
        return &burrow_type_SqlNullInt16;
    if (str_eq(typ, BURROW_S("int32")))
        return TYPE_INT32;
    if (str_eq(typ, BURROW_S("nullint32")))
        return &burrow_type_SqlNullInt32;
    if (str_eq(typ, BURROW_S("string")))
        return TYPE_STRING;
    if (str_eq(typ, BURROW_S("nullstring")))
        return &burrow_type_SqlNullString;
    if (str_eq(typ, BURROW_S("int64")))
        return TYPE_INT64;
    if (str_eq(typ, BURROW_S("nullint64")))
        return &burrow_type_SqlNullInt64;
    if (str_eq(typ, BURROW_S("float64")))
        return TYPE_FLOAT64;
    if (str_eq(typ, BURROW_S("nullfloat64")))
        return &burrow_type_SqlNullFloat64;
    if (str_eq(typ, BURROW_S("datetime")))
        return TYPE_TIME;
    if (str_eq(typ, BURROW_S("any")))
        return TYPE_ANY;
    panic_str(
        fmt_sprintf_v(error_allocator(), "invalid fakedb column type of %s", typ));
}

/* ------------------------------------------------------------- cursor */

typedef struct FakeCursor {
    FakeDB *db;
    FakeConn *parent_mem;
    Arena arena;
    Int nsets;
    Slice *cols;    /* per set, a Slice of Str */
    Str **col_type; /* per set */
    Any ***rows;    /* per set, nrows of them */
    Int *nrows;
    Int pos_set;
    Int pos_row;
    bool closed;

    /* err_pos and err are for making next return early with an error. */
    Int err_pos;
    Error err;

    /* Data returned to clients. We clone and stash it here so it can be
     * invalidated by close and next. */
    Bytes *owned;
    Int nowned;
    Int capowned;

    int64_t line;

    /* close_err is returned by close. */
    Error close_err;

    /* A cursor stored in a table outlives its close, the way Go's collector
     * keeps it. */
    bool in_table;
} FakeCursor;

static Alloc *fdb_cursor_alloc(FakeCursor *rc) {
    return arena_allocator(&rc->arena);
}

static void fdb_cursor_touch(FakeCursor *rc) {
    fdb_touch(rc->parent_mem);
    rc->line++;
}

static void fdb_cursor_invalidate(FakeCursor *rc) {
    for (Int i = 0; i < rc->nowned; i++) {
        Byte *p = rc->owned[i].p;
        for (Int j = 0; j < rc->owned[i].len; j++)
            p[j] = 'x';
    }
    rc->nowned = 0;
}

static void fdb_cursor_free(FakeCursor *rc) {
    arena_free(&rc->arena);
    mem_free(heap_allocator(), rc, sizeof(FakeCursor), _Alignof(FakeCursor));
}

static Error fdb_cursor_close(void *self) {
    FakeCursor *rc = self;
    fdb_cursor_touch(rc);
    fdb_touch(rc->parent_mem);
    fdb_cursor_invalidate(rc);
    rc->closed = true;
    Error e = rc->close_err;
    if (!rc->in_table)
        fdb_cursor_free(rc);
    return e;
}

static Slice fdb_cursor_columns(void *self) {
    FakeCursor *rc = self;
    return rc->cols[rc->pos_set];
}

static const Type *fdb_cursor_column_type_scan_type(void *self, Int index) {
    FakeCursor *rc = self;
    return fdb_col_type_to_type(rc->col_type[rc->pos_set][index]);
}

static Error fdb_cursor_next(void *self, Slice dest) {
    if (fdb_rows_cursor_next_hook != NULL)
        return fdb_rows_cursor_next_hook(dest);
    FakeCursor *rc = self;
    if (rc->closed)
        return fmt_errorf_v("fakedb: cursor is closed");
    fdb_cursor_touch(rc);
    rc->pos_row++;
    if (rc->pos_row == rc->err_pos)
        return rc->err;
    if (rc->pos_row >= rc->nrows[rc->pos_set])
        return io_eof; /* per interface spec */
    /* Corrupt any previously returned bytes. */
    fdb_cursor_invalidate(rc);
    Any *row = rc->rows[rc->pos_set][rc->pos_row];
    Any *d = dest.p;
    Alloc *a = fdb_cursor_alloc(rc);
    for (Int i = 0; i < rc->cols[rc->pos_set].len; i++) {
        Any v = row[i];
        if (fdb_is_bytes(v.t)) {
            /* Clone bytes and stash for later invalidation. */
            const Bytes *b = v.data;
            Bytes *bs = BURROW_NEW(a, Bytes);
            if (bs == NULL)
                return burrow_err_out_of_memory;
            *bs = *b;
            if (b->p != NULL) {
                *bs = bytes_clone(a, *b);
                if (b->len > 0 && bs->p == NULL)
                    return burrow_err_out_of_memory;
            }
            if (rc->nowned == rc->capowned) {
                Int n = rc->capowned == 0 ? 8 : rc->capowned * 2;
                Bytes *grown = BURROW_NEW_N(a, Bytes, (size_t)n);
                if (grown == NULL)
                    return burrow_err_out_of_memory;
                if (rc->nowned > 0)
                    memcpy(grown, rc->owned, (size_t)rc->nowned * sizeof(Bytes));
                rc->owned = grown;
                rc->capowned = n;
            }
            rc->owned[rc->nowned++] = *bs;
            v = (Any){v.t, bs};
        }
        d[i] = v;
    }
    return BURROW_NO_ERROR;
}

static bool fdb_cursor_has_next_result_set(void *self) {
    FakeCursor *rc = self;
    fdb_cursor_touch(rc);
    return rc->pos_set < rc->nsets - 1;
}

static Error fdb_cursor_next_result_set(void *self) {
    FakeCursor *rc = self;
    fdb_cursor_touch(rc);
    if (fdb_cursor_has_next_result_set(rc)) {
        rc->pos_set++;
        rc->pos_row = -1;
        return BURROW_NO_ERROR;
    }
    return io_eof; /* per interface spec */
}

static const DriverRowsVT fdb_cursor_vt = {
    .columns = fdb_cursor_columns,
    .close = fdb_cursor_close,
    .next = fdb_cursor_next,
    .has_next_result_set = fdb_cursor_has_next_result_set,
    .next_result_set = fdb_cursor_next_result_set,
    .column_type_scan_type = fdb_cursor_column_type_scan_type,
};

static FakeCursor *fdb_cursor_new(FakeConn *c, Int nsets) {
    FakeCursor *rc = BURROW_NEW(heap_allocator(), FakeCursor);
    if (rc == NULL)
        panic_str(BURROW_S("fakedb: out of memory"));
    arena_init(&rc->arena, heap_allocator(), 0);
    Alloc *a = fdb_cursor_alloc(rc);
    rc->db = c->db;
    rc->parent_mem = c;
    rc->pos_row = -1;
    rc->err_pos = -1;
    rc->nsets = nsets;
    rc->cols = BURROW_NEW_N(a, Slice, (size_t)nsets);
    rc->col_type = BURROW_NEW_N(a, Str *, (size_t)nsets);
    rc->rows = BURROW_NEW_N(a, Any **, (size_t)nsets);
    rc->nrows = BURROW_NEW_N(a, Int, (size_t)nsets);
    if (rc->cols == NULL || rc->col_type == NULL || rc->rows == NULL ||
        rc->nrows == NULL)
        panic_str(BURROW_S("fakedb: out of memory"));
    return rc;
}

/* Who owns a FakeCursor, for the tests that reach into one. */
static FakeCursor *fdb_get_rows_cursor(SqlRows *rs);

/* ------------------------------------------------------ stmt, continued */

static DriverStmt fdb_stmt_as_driver(FakeStmt *s);
static DriverRows fdb_stmt_query_context(void *self, Context ctx, Slice args,
                                         Error *err);

/* parts are table|col=?,col2=val */
static Error fdb_prepare_insert(FakeConn *c, Context ctx, FakeStmt *s, Slice parts);

static FakeStmt *fdb_conn_prepare(FakeConn *c, Context ctx, Str query, Error *err);

static Error fdb_prepare_insert(FakeConn *c, Context ctx, FakeStmt *s, Slice parts) {
    if (parts.len != 2) {
        (void)fdb_stmt_close_go(s);
        return fmt_errorf_v("fakedb: invalid INSERT syntax with %d parts; want 2",
                            parts.len);
    }
    Alloc *a = fdb_stmt_alloc(s);
    s->table = fdb_part(parts, 0);
    Slice specs = fdb_split(s, fdb_part(parts, 1), ",");
    s->col_name = BURROW_NEW_N(a, Str, (size_t)specs.len + 1);
    s->col_value = BURROW_NEW_N(a, Any, (size_t)specs.len + 1);
    s->placeholder_converter =
        BURROW_NEW_N(a, DriverValueConverter, (size_t)specs.len + 1);
    for (Int n = 0; n < specs.len; n++) {
        Str colspec = fdb_part(specs, n);
        Slice name_val = fdb_split(s, colspec, "=");
        if (name_val.len != 2) {
            (void)fdb_stmt_close_go(s);
            return fmt_errorf_v(
                "fakedb: INSERT table %q has invalid column spec of %q (index %d)",
                s->table, colspec, n);
        }
        Str column = fdb_part(name_val, 0), value = fdb_part(name_val, 1);
        Str ctype = BURROW_STR_EMPTY;
        if (!fdb_column_type(c->db, s->table, column, &ctype)) {
            (void)fdb_stmt_close_go(s);
            return fmt_errorf_v(
                "fakedb: INSERT table %q references non-existent column %q", s->table,
                column);
        }
        s->col_name[s->ncols] = column;

        if (!strings_has_prefix(value, BURROW_S("?"))) {
            Any subset = {NULL, NULL};
            /* Convert to driver subset type. */
            if (str_eq(ctype, BURROW_S("string")) || str_eq(ctype, BURROW_S("blob"))) {
                Bytes *b = BURROW_NEW(a, Bytes);
                *b = slice_from((void *)(uintptr_t)value.p, value.len, value.len,
                                TYPE_BYTE);
                subset = (Any){TYPE_BYTES, b};
            } else if (str_eq(ctype, BURROW_S("int32"))) {
                Error e = BURROW_NO_ERROR;
                Int i = strconv_atoi(value, &e);
                if (BURROW_FAILED(e)) {
                    (void)fdb_stmt_close_go(s);
                    return fmt_errorf_v("fakedb: invalid conversion to int32 from %q",
                                        value);
                }
                int64_t *p = BURROW_NEW(a, int64_t);
                *p = (int64_t)i; /* int64 is a subset type, but not int32 */
                subset = (Any){TYPE_INT64, p};
            } else if (str_eq(ctype,
                              BURROW_S("table"))) { /* For testing cursor reads. */
                c->skip_dirty_session = true;
                Str vparts_rest = BURROW_STR_EMPTY;
                bool found = false;
                Str vtable = strings_cut(value, BURROW_S("!"), &vparts_rest, &found);
                Slice rest = fdb_split(s, vparts_rest, "!");
                Str cols =
                    found ? strings_join(a, rest, BURROW_S(",")) : BURROW_STR_EMPTY;
                Str q = fmt_sprintf_v(a, "SELECT|%s|%s|", vtable, cols);
                Error e = BURROW_NO_ERROR;
                FakeStmt *sub = fdb_conn_prepare(c, ctx, q, &e);
                if (BURROW_FAILED(e))
                    return e;
                DriverRows cursor = fdb_stmt_query_context(
                    sub, ctx, slice_from(NULL, 0, 0, TYPE_DRIVER_NAMED_VALUE), &e);
                (void)fdb_stmt_close(sub);
                if (BURROW_FAILED(e))
                    return e;
                ((FakeCursor *)cursor.data)->in_table = true;
                DriverRows *keep = fdb_keep(sizeof(DriverRows));
                *keep = cursor;
                subset = (Any){&burrow_type_DriverRows, keep};
            } else {
                (void)fdb_stmt_close_go(s);
                return fmt_errorf_v("fakedb: unsupported conversion for pre-bound "
                                    "parameter %q to type %q",
                                    value, ctype);
            }
            s->col_value[s->ncols] = subset;
        } else {
            s->placeholders++;
            s->placeholder_converter[s->nconv++] = fdb_converter_for_type(ctype);
            Str *p = BURROW_NEW(a, Str);
            *p = value;
            s->col_value[s->ncols] = (Any){TYPE_STRING, p};
        }
        s->ncols++;
    }
    return BURROW_NO_ERROR;
}

static FakeStmt *fdb_conn_prepare(FakeConn *c, Context ctx, Str query, Error *err) {
    c->num_prepare++;
    if (c->db == NULL)
        panic_str(BURROW_S("nil c.db"));

    if (c->sticky_bad ||
        (fdb_hook_prepare_bad_conn != NULL && fdb_hook_prepare_bad_conn())) {
        *err = fdb_error("Prepare: Sticky Bad", driver_err_bad_conn);
        return NULL;
    }

    fdb_touch(c);
    *err = BURROW_NO_ERROR;
    FakeStmt *first = NULL, *prev = NULL;
    Arena tmp;
    arena_init(&tmp, heap_allocator(), 0);
    Slice queries = strings_split(arena_allocator(&tmp), query, BURROW_S(";"));
    for (Int qi = 0; qi < queries.len; qi++) {
        FakeStmt *stmt = fdb_stmt_new(c, fdb_part(queries, qi));
        Slice parts = fdb_split(stmt, stmt->q, "|");
        if (parts.len >= 3) {
            if (str_eq(fdb_part(parts, 0), BURROW_S("PANIC"))) {
                stmt->panic = fdb_part(parts, 1);
                parts = slice_sub(parts, 2, parts.len);
            } else if (str_eq(fdb_part(parts, 0), BURROW_S("WAIT"))) {
                Error e = BURROW_NO_ERROR;
                Duration wait = time_parse_duration(fdb_part(parts, 1), &e);
                if (BURROW_FAILED(e)) {
                    *err = fmt_errorf_v("fakedb: expected section after WAIT to be a "
                                        "duration, got %q %v",
                                        fdb_part(parts, 1), e);
                    fdb_stmt_free(stmt);
                    break;
                }
                parts = slice_sub(parts, 2, parts.len);
                stmt->wait = wait;
            }
        }
        Str cmd = fdb_part(parts, 0);
        stmt->cmd = cmd;
        parts = slice_sub(parts, 1, parts.len);

        if (c->waiter != NULL) {
            c->waiter(ctx);
            Error e = context_err(ctx);
            if (BURROW_FAILED(e)) {
                *err = e;
                fdb_stmt_free(stmt);
                break;
            }
        }

        if (stmt->wait > 0) {
            TimeTimer *wait = time_new_timer(heap_allocator(), stmt->wait);
            SelectCase cases[2] = {BURROW_RECV(time_timer_c(wait), NULL),
                                   BURROW_RECV(context_done(ctx), NULL)};
            Int which = chan_select(cases, 2);
            (void)time_timer_stop(wait);
            time_timer_free(wait);
            if (which == 1) {
                *err = context_err(ctx);
                fdb_stmt_free(stmt);
                break;
            }
        }

        fdb_incr_stat(c, &c->stmts_made);
        Error e = BURROW_NO_ERROR;
        if (str_eq(cmd, BURROW_S("WIPE"))) {
            /* Nothing */
        } else if (str_eq(cmd, BURROW_S("SELECT"))) {
            e = fdb_prepare_select(c, stmt, parts);
        } else if (str_eq(cmd, BURROW_S("CREATE"))) {
            e = fdb_prepare_create(stmt, parts);
        } else if (str_eq(cmd, BURROW_S("INSERT")) || str_eq(cmd, BURROW_S("NOSERT"))) {
            /* NOSERT does all the prep-work like for an INSERT but doesn't
             * actually insert the row. Used for some of the concurrent tests. */
            e = fdb_prepare_insert(c, ctx, stmt, parts);
        } else {
            (void)fdb_stmt_close_go(stmt);
            e = fmt_errorf_v("fakedb: unsupported command type %q", cmd);
        }
        if (BURROW_FAILED(e)) {
            *err = e;
            fdb_stmt_free(stmt);
            break;
        }
        if (first == NULL)
            first = stmt;
        if (prev != NULL)
            prev->next = stmt;
        prev = stmt;
    }
    arena_free(&tmp);
    if (BURROW_FAILED(*err)) {
        fdb_stmt_free(first);
        return NULL;
    }
    return first;
}

static DriverStmt fdb_conn_prepare_context(void *self, Context ctx, Str query,
                                           Error *err) {
    FakeStmt *s = fdb_conn_prepare(self, ctx, query, err);
    if (s == NULL)
        return (DriverStmt){NULL, NULL};
    return fdb_stmt_as_driver(s);
}

static DriverValueConverter fdb_stmt_column_converter(void *self, Int idx) {
    FakeStmt *s = self;
    if (str_eq(s->panic, BURROW_S("ColumnConverter")))
        panic_str(s->panic);
    if (s->nconv == 0)
        return driver_default_parameter_converter;
    return s->placeholder_converter[idx];
}

static Any fdb_value_from_placeholder_name(Slice args, Str name) {
    const DriverNamedValue *nv = args.p;
    for (Int i = 0; i < args.len; i++) {
        if (str_eq(nv[i].name, name))
            return nv[i].value;
    }
    return (Any){NULL, NULL};
}

/* When do_insert is true, add the row to the table. When do_insert is false,
 * do prep-work and error checking, but don't actually add the row. */
static DriverResult fdb_stmt_exec_insert(FakeStmt *s, Alloc *a, Slice args,
                                         bool do_insert, Error *err) {
    FakeDB *db = s->c->db;
    if (args.len != s->placeholders)
        panic_str(BURROW_S("error in pkg db; should only get here if size is correct"));
    sync_mutex_lock(&db->mu);
    FakeTable *t = fdb_table(db, s->table);
    sync_mutex_unlock(&db->mu);
    if (t == NULL) {
        *err = fmt_errorf_v("fakedb: table %q doesn't exist", s->table);
        return (DriverResult){NULL, NULL};
    }

    sync_mutex_lock(&t->mu);
    Any *cols = NULL;
    if (do_insert)
        cols = fdb_keep((size_t)t->ncols * sizeof(Any) + 1);
    const DriverNamedValue *nv = args.p;
    Int arg_pos = 0;
    for (Int n = 0; n < s->ncols; n++) {
        Int colidx = fdb_column_index(t, s->col_name[n]);
        if (colidx == -1) {
            sync_mutex_unlock(&t->mu);
            *err = fmt_errorf_v("fakedb: column %q doesn't exist or dropped since "
                                "prepared statement was created",
                                s->col_name[n]);
            return (DriverResult){NULL, NULL};
        }
        Any val = {NULL, NULL};
        Any cv = s->col_value[n];
        if (cv.t == TYPE_STRING &&
            strings_has_prefix(*(const Str *)cv.data, BURROW_S("?"))) {
            Str strvalue = *(const Str *)cv.data;
            if (strvalue.len == 1)
                val = nv[arg_pos].value;
            else /* Assign value from argument placeholder name. */
                val = fdb_value_from_placeholder_name(
                    args, (Str){strvalue.p + 1, strvalue.len - 1});
            arg_pos++;
        } else {
            val = cv;
        }
        if (do_insert)
            cols[colidx] = fdb_keep_value(val);
    }

    if (do_insert) {
        if (t->nrows == t->caprows) {
            Int n = t->caprows == 0 ? 8 : t->caprows * 2;
            Any **grown = fdb_keep((size_t)n * sizeof(Any *));
            if (t->nrows > 0)
                memcpy(grown, t->rows, (size_t)t->nrows * sizeof(Any *));
            t->rows = grown;
            t->caprows = n;
        }
        t->rows[t->nrows++] = cols;
    }
    sync_mutex_unlock(&t->mu);
    DriverRowsAffected *ra = BURROW_NEW(a, DriverRowsAffected);
    if (ra == NULL) {
        *err = burrow_err_out_of_memory;
        return (DriverResult){NULL, NULL};
    }
    *ra = 1;
    *err = BURROW_NO_ERROR;
    return driver_rows_affected_as_result(ra);
}

static DriverResult fdb_stmt_exec_context(void *self, Alloc *a, Context ctx, Slice args,
                                          Error *err) {
    FakeStmt *s = self;
    DriverResult none = {NULL, NULL};
    if (str_eq(s->panic, BURROW_S("Exec")))
        panic_str(s->panic);
    if (s->closed) {
        *err = fmt_errorf_v("fakedb: statement has been closed");
        return none;
    }
    if (s->c->sticky_bad ||
        (fdb_hook_exec_bad_conn != NULL && fdb_hook_exec_bad_conn())) {
        *err = fdb_error("Exec: Sticky Bad", driver_err_bad_conn);
        return none;
    }
    if (fdb_conn_is_dirty_and_mark(s->c)) {
        *err = fmt_errorf_v("fakedb: session is dirty");
        return none;
    }
    *err = fdb_check_subset_types(s->c->db->allow_any, args);
    if (BURROW_FAILED(*err))
        return none;
    fdb_touch(s->c);

    if (s->wait > 0)
        time_sleep(s->wait);

    bool ok = false;
    if (chan_try_recv(context_done(ctx), NULL, &ok)) {
        *err = context_err(ctx);
        return none;
    }

    FakeDB *db = s->c->db;
    if (str_eq(s->cmd, BURROW_S("WIPE"))) {
        fdb_wipe(db);
        return driver_result_no_rows;
    }
    if (str_eq(s->cmd, BURROW_S("CREATE"))) {
        *err = fdb_create_table(db, s->table, s->ncols, s->col_name, s->col_type);
        if (BURROW_FAILED(*err))
            return none;
        return driver_result_no_rows;
    }
    if (str_eq(s->cmd, BURROW_S("INSERT")))
        return fdb_stmt_exec_insert(s, a, args, true, err);
    if (str_eq(s->cmd, BURROW_S("NOSERT")))
        return fdb_stmt_exec_insert(s, a, args, false, err);
    *err =
        fmt_errorf_v("fakedb: unimplemented statement Exec command type of %q", s->cmd);
    return none;
}

static bool fdb_where_matches(const FakeTable *t, Any *trow, const FakeStmt *s,
                              Slice args, Error *err) {
    Arena tmp;
    arena_init(&tmp, heap_allocator(), 0);
    Alloc *a = arena_allocator(&tmp);
    const DriverNamedValue *nv = args.p;
    bool match = true;
    for (Int w = 0; match && w < s->nwhere; w++) {
        const FakeBoundCol *wcol = &s->where_col[w];
        Int idx = fdb_column_index(t, wcol->column);
        if (idx == -1) {
            *err = fmt_errorf_v("fakedb: invalid where clause column %v", wcol->column);
            match = false;
            break;
        }
        Any tcol = trow[idx];
        Str bs_str;
        if (fdb_is_bytes(tcol.t)) {
            /* lazy hack to avoid sprintf %v on a []byte */
            const Bytes *b = tcol.data;
            bs_str = (Str){b->p, b->len};
            tcol = (Any){TYPE_STRING, &bs_str};
        }
        Any arg = {NULL, NULL};
        if (str_eq(wcol->placeholder, BURROW_S("?")))
            arg = nv[wcol->ordinal - 1].value;
        else
            arg = fdb_value_from_placeholder_name(
                args, (Str){wcol->placeholder.p + 1, wcol->placeholder.len - 1});
        if (!str_eq(fmt_sprintf_v(a, "%v", tcol), fmt_sprintf_v(a, "%v", arg)))
            match = false;
    }
    arena_free(&tmp);
    return match;
}

static DriverRows fdb_stmt_query_context(void *self, Context ctx, Slice args,
                                         Error *err) {
    (void)ctx;
    FakeStmt *s = self;
    DriverRows none = {NULL, NULL};
    if (str_eq(s->panic, BURROW_S("Query")))
        panic_str(s->panic);
    if (s->closed) {
        *err = fmt_errorf_v("fakedb: statement has been closed");
        return none;
    }
    if (s->c->sticky_bad ||
        (fdb_hook_query_bad_conn != NULL && fdb_hook_query_bad_conn())) {
        *err = fdb_error("Query: Sticky Bad", driver_err_bad_conn);
        return none;
    }
    if (fdb_conn_is_dirty_and_mark(s->c)) {
        *err = fmt_errorf_v("fakedb: session is dirty");
        return none;
    }
    *err = fdb_check_subset_types(s->c->db->allow_any, args);
    if (BURROW_FAILED(*err))
        return none;

    fdb_touch(s->c);
    FakeDB *db = s->c->db;
    if (args.len != s->placeholders)
        panic_str(BURROW_S("error in pkg db; should only get here if size is correct"));

    Int nsets = 0;
    for (FakeStmt *p = s; p != NULL; p = p->next)
        nsets++;
    FakeCursor *rc = fdb_cursor_new(s->c, nsets);
    Alloc *a = fdb_cursor_alloc(rc);
    const DriverNamedValue *nv = args.p;

    for (Int set = 0; s != NULL; set++, s = s->next) {
        sync_mutex_lock(&db->mu);
        FakeTable *t = fdb_table(db, s->table);
        sync_mutex_unlock(&db->mu);
        if (t == NULL) {
            *err = fmt_errorf_v("fakedb: table %q doesn't exist", s->table);
            fdb_cursor_free(rc);
            return none;
        }

        if (str_eq(s->table, BURROW_S("magicquery"))) {
            if (s->nwhere == 2 && str_eq(s->where_col[0].column, BURROW_S("op")) &&
                str_eq(s->where_col[1].column, BURROW_S("millis"))) {
                if (fdb_str_is(nv[0].value, "sleep"))
                    time_sleep((Duration)(*(const int64_t *)nv[1].value.data) *
                               TIME_MILLISECOND);
            }
        }
        if (str_eq(s->table, BURROW_S("tx_status")) && s->ncols > 0 &&
            str_eq(s->col_name[0], BURROW_S("tx_status"))) {
            fdb_cursor_free(rc);
            FakeCursor *c1 = fdb_cursor_new(s->c, 1);
            Alloc *ca = fdb_cursor_alloc(c1);
            Str *status = BURROW_NEW(ca, Str);
            *status = s->c->curr_tx != NULL ? BURROW_S("transaction")
                                            : BURROW_S("autocommit");
            Any *row = BURROW_NEW_N(ca, Any, 1);
            row[0] = (Any){TYPE_STRING, status};
            c1->rows[0] = BURROW_NEW_N(ca, Any *, 1);
            c1->rows[0][0] = row;
            c1->nrows[0] = 1;
            Str *name = BURROW_NEW_N(ca, Str, 1);
            name[0] = BURROW_S("tx_status");
            c1->cols[0] = slice_from(name, 1, 1, TYPE_STRING);
            Str *typ = BURROW_NEW_N(ca, Str, 1);
            typ[0] = BURROW_S("string");
            c1->col_type[0] = typ;
            return (DriverRows){&fdb_cursor_vt, c1};
        }

        sync_mutex_lock(&t->mu);

        Int *col_idx = BURROW_NEW_N(a, Int, (size_t)s->ncols + 1);
        for (Int i = 0; i < s->ncols; i++) {
            Int idx = fdb_column_index(t, s->col_name[i]);
            if (idx == -1) {
                sync_mutex_unlock(&t->mu);
                *err = fmt_errorf_v("fakedb: unknown column name %q", s->col_name[i]);
                fdb_cursor_free(rc);
                return none;
            }
            col_idx[i] = idx;
        }

        Any **mrows = BURROW_NEW_N(a, Any *, (size_t)t->nrows + 1);
        Int nm = 0;
        for (Int r = 0; r < t->nrows; r++) {
            /* Process the where clause, skipping non-match rows. This is lazy
             * and just uses %v to test equality. Good enough for test code. */
            Error e = BURROW_NO_ERROR;
            bool match = fdb_where_matches(t, t->rows[r], s, args, &e);
            if (BURROW_FAILED(e)) {
                sync_mutex_unlock(&t->mu);
                *err = e;
                fdb_cursor_free(rc);
                return none;
            }
            if (!match)
                continue;
            Any *mrow = BURROW_NEW_N(a, Any, (size_t)s->ncols + 1);
            for (Int i = 0; i < s->ncols; i++)
                mrow[i] = t->rows[r][col_idx[i]];
            mrows[nm++] = mrow;
        }

        Str *names = BURROW_NEW_N(a, Str, (size_t)s->ncols + 1);
        Str *types = BURROW_NEW_N(a, Str, (size_t)s->ncols + 1);
        for (Int i = 0; i < s->ncols; i++) {
            names[i] = str_clone(a, s->col_name[i]);
            types[i] = str_clone(a, t->coltype[col_idx[i]]);
        }

        sync_mutex_unlock(&t->mu);

        rc->rows[set] = mrows;
        rc->nrows[set] = nm;
        rc->cols[set] = slice_from(names, s->ncols, s->ncols, TYPE_STRING);
        rc->col_type[set] = types;
    }

    *err = BURROW_NO_ERROR;
    return (DriverRows){&fdb_cursor_vt, rc};
}

static Int fdb_stmt_num_input(void *self) {
    FakeStmt *s = self;
    if (str_eq(s->panic, BURROW_S("NumInput")))
        panic_str(s->panic);
    return s->placeholders;
}

static const DriverStmtVT fdb_stmt_vt = {
    .close = fdb_stmt_close,
    .num_input = fdb_stmt_num_input,
    .exec_context = fdb_stmt_exec_context,
    .query_context = fdb_stmt_query_context,
    .column_converter = fdb_stmt_column_converter,
};

static DriverStmt fdb_stmt_as_driver(FakeStmt *s) {
    return (DriverStmt){&fdb_stmt_vt, s};
}

static const DriverConnVT fdb_conn_vt = {
    .close = fdb_conn_close,
    .begin = fdb_conn_begin,
    .prepare_context = fdb_conn_prepare_context,
    .exec_context = fdb_conn_exec_context,
    .query_context = fdb_conn_query_context,
    .reset_session = fdb_conn_reset_session,
    .is_valid = fdb_conn_is_valid,
};

/* ------------------------------------------------------------- driver */

/* Supports dsn forms:
 *
 *     <dbname>
 *     <dbname>;<opts>  (only currently supported option is `badConn`, which
 *                       causes driver_err_bad_conn to be returned on every
 *                       other conn begin) */
static FakeConn *fdb_open(Str dsn, Error *err) {
    sync_mutex_lock(&fdb_hook_open_err_mu);
    Error (*fn)(void) = fdb_hook_open_err;
    sync_mutex_unlock(&fdb_hook_open_err_mu);
    if (fn != NULL) {
        Error e = fn();
        if (BURROW_FAILED(e)) {
            *err = e;
            return NULL;
        }
    }
    Str opts = BURROW_STR_EMPTY;
    bool found = false;
    Str name = strings_cut(dsn, BURROW_S(";"), &opts, &found);

    FakeDB *db = fdb_get_db(name);

    sync_mutex_lock(&fdb_mu);
    fdb_open_count++;
    sync_mutex_unlock(&fdb_mu);
    FakeConn *conn = BURROW_NEW(heap_allocator(), FakeConn);
    if (conn == NULL)
        panic_str(BURROW_S("fakedb: out of memory"));
    conn->db = db;
    if (found && strings_has_prefix(opts, BURROW_S("badConn")))
        conn->bad = true;
    if (fdb_wait_ch != NULL) {
        chan_send(fdb_waiting_ch, &(Byte){0});
        chan_recv(fdb_wait_ch, NULL);
        fdb_wait_ch = NULL;
        fdb_waiting_ch = NULL;
    }
    *err = BURROW_NO_ERROR;
    return conn;
}

static DriverConn fdb_driver_open(void *self, Str name, Error *err) {
    (void)self;
    FakeConn *c = fdb_open(name, err);
    if (c == NULL)
        return (DriverConn){NULL, NULL};
    return (DriverConn){&fdb_conn_vt, c};
}

static const DriverVT fdb_driver_vt = {NULL, fdb_driver_open, NULL};
static const Driver fdriver = {&fdb_driver_vt, NULL};

static FakeConn *fdb_get_fake_conn(DriverConn c) {
    return c.data;
}

/* ---------------------------------------------------------- connectors */

/* Which of sql_test.go's connectors this is: fakeConnector, basicConnector
 * or rowsColumnScannerConnector. */
typedef enum FakeKind { FAKE_DEFAULT, FAKE_BASIC, FAKE_SCANCOLS } FakeKind;

typedef struct FakeConnector {
    Str name;
    FakeWaiter waiter;
    bool closed;
    FakeKind kind;
} FakeConnector;

/* basicConn implements only the bare minimum of a DriverConn. */
typedef struct FakeBasicStmt {
    FakeConn *fc;
    FakeStmt *s;
} FakeBasicStmt;

static Slice fdb_basic_values_to_named(FakeBasicStmt *bs, Alloc *a, Slice args,
                                       Error *err) {
    DriverNamedValue *nv = BURROW_NEW_N(a, DriverNamedValue, (size_t)args.len + 1);
    if (nv == NULL) {
        *err = burrow_err_out_of_memory;
        return slice_from(NULL, 0, 0, TYPE_DRIVER_NAMED_VALUE);
    }
    const DriverValue *v = args.p;
    for (Int i = 0; i < args.len; i++) {
        DriverValueConverter cv = fdb_stmt_column_converter(bs->s, i);
        Error e = BURROW_NO_ERROR;
        DriverValue val = cv.vt->convert_value(cv.data, a, v[i], &e);
        if (BURROW_FAILED(e)) {
            *err = fmt_errorf_v("sql: converting argument $%v type: %w", i + 1, e);
            return slice_from(NULL, 0, 0, TYPE_DRIVER_NAMED_VALUE);
        }
        nv[i] = (DriverNamedValue){BURROW_STR_EMPTY, i + 1, val};
    }
    *err = BURROW_NO_ERROR;
    return slice_from(nv, args.len, args.len, TYPE_DRIVER_NAMED_VALUE);
}

static DriverResult fdb_basic_exec(void *self, Alloc *a, Slice args, Error *err) {
    FakeBasicStmt *bs = self;
    Slice nvs = fdb_basic_values_to_named(bs, a, args, err);
    if (BURROW_FAILED(*err))
        return (DriverResult){NULL, NULL};
    return fdb_stmt_exec_context(bs->s, a, context_background(), nvs, err);
}

static DriverRows fdb_basic_query(void *self, Slice args, Error *err) {
    FakeBasicStmt *bs = self;
    Arena tmp;
    arena_init(&tmp, heap_allocator(), 0);
    Slice nvs = fdb_basic_values_to_named(bs, arena_allocator(&tmp), args, err);
    DriverRows rows = {NULL, NULL};
    if (!BURROW_FAILED(*err))
        rows = fdb_stmt_query_context(bs->s, context_background(), nvs, err);
    arena_free(&tmp);
    return rows;
}

static Error fdb_basic_stmt_close(void *self) {
    FakeBasicStmt *bs = self;
    Error e = fdb_stmt_close(bs->s);
    mem_free(heap_allocator(), bs, sizeof(FakeBasicStmt), _Alignof(FakeBasicStmt));
    return e;
}

static Int fdb_basic_num_input(void *self) {
    return fdb_stmt_num_input(((FakeBasicStmt *)self)->s);
}

static const DriverStmtVT fdb_basic_stmt_vt = {
    .close = fdb_basic_stmt_close,
    .num_input = fdb_basic_num_input,
    .exec = fdb_basic_exec,
    .query = fdb_basic_query,
};

static DriverStmt fdb_basic_prepare(void *self, Str query, Error *err) {
    FakeConn *c = self;
    FakeStmt *s = fdb_conn_prepare(c, context_background(), query, err);
    if (s == NULL)
        return (DriverStmt){NULL, NULL};
    FakeBasicStmt *bs = BURROW_NEW(heap_allocator(), FakeBasicStmt);
    if (bs == NULL)
        panic_str(BURROW_S("fakedb: out of memory"));
    bs->fc = c;
    bs->s = s;
    return (DriverStmt){&fdb_basic_stmt_vt, bs};
}

static const DriverConnVT fdb_basic_conn_vt = {
    .prepare = fdb_basic_prepare,
    .close = fdb_conn_close,
    .begin = fdb_conn_begin,
};

/* rowsColumnScannerConn: a conn whose rows set next_row and scan_column. */
typedef struct FakeScanRows {
    FakeCursor *rc;
    Any *row;
    Int nrow;
} FakeScanRows;

/* sql_test.go's testStringType, which ScanColumn fills in itself. */
#define TEST_STRING_TYPE_FIELDS(F, T) F(T, Str, s, "")
BURROW_STRUCT(TestStringType, TEST_STRING_TYPE_FIELDS);

static Error fdb_scan_rows_next_row(void *self) {
    FakeScanRows *r = self;
    if (r->row == NULL) {
        r->nrow = fdb_cursor_columns(r->rc).len;
        r->row = BURROW_NEW_N(fdb_cursor_alloc(r->rc), Any, (size_t)r->nrow + 1);
        if (r->row == NULL)
            return burrow_err_out_of_memory;
    }
    return fdb_cursor_next(r->rc, slice_from(r->row, r->nrow, r->nrow, TYPE_ANY));
}

static Error fdb_scan_rows_next_result_set(void *self) {
    FakeScanRows *r = self;
    r->row = NULL;
    return fdb_cursor_next_result_set(r->rc);
}

static Error fdb_scan_rows_scan_column(void *self, DriverScanContext ctx, Int index,
                                       Any dest) {
    FakeScanRows *r = self;
    if (index < 0 || index >= r->nrow)
        return fmt_errorf_v("index %v out of range", index);
    if (dest.t == TYPE_OF(TestStringType)) {
        TestStringType *d = dest.data;
        Any v = r->row[index];
        if (v.t == TYPE_STRING || fdb_is_bytes(v.t))
            return sql_convert_assign(NULL, ctx, (Any){TYPE_STRING, &d->s}, v);
    }
    return sql_convert_assign(NULL, ctx, dest, r->row[index]);
}

static Slice fdb_scan_rows_columns(void *self) {
    return fdb_cursor_columns(((FakeScanRows *)self)->rc);
}

static Error fdb_scan_rows_close(void *self) {
    FakeScanRows *r = self;
    Error e = fdb_cursor_close(r->rc);
    mem_free(heap_allocator(), r, sizeof(FakeScanRows), _Alignof(FakeScanRows));
    return e;
}

static Error fdb_scan_rows_next(void *self, Slice dest) {
    return fdb_cursor_next(((FakeScanRows *)self)->rc, dest);
}

static bool fdb_scan_rows_has_next_result_set(void *self) {
    return fdb_cursor_has_next_result_set(((FakeScanRows *)self)->rc);
}

static const Type *fdb_scan_rows_column_type_scan_type(void *self, Int index) {
    return fdb_cursor_column_type_scan_type(((FakeScanRows *)self)->rc, index);
}

static const DriverRowsVT fdb_scan_rows_vt = {
    .columns = fdb_scan_rows_columns,
    .close = fdb_scan_rows_close,
    .next = fdb_scan_rows_next,
    .next_row = fdb_scan_rows_next_row,
    .scan_column = fdb_scan_rows_scan_column,
    .has_next_result_set = fdb_scan_rows_has_next_result_set,
    .next_result_set = fdb_scan_rows_next_result_set,
    .column_type_scan_type = fdb_scan_rows_column_type_scan_type,
};

static DriverRows fdb_scan_stmt_query_context(void *self, Context ctx, Slice args,
                                              Error *err) {
    DriverRows rows = fdb_stmt_query_context(self, ctx, args, err);
    if (BURROW_FAILED(*err))
        return rows;
    FakeScanRows *r = BURROW_NEW(heap_allocator(), FakeScanRows);
    if (r == NULL)
        panic_str(BURROW_S("fakedb: out of memory"));
    r->rc = rows.data;
    return (DriverRows){&fdb_scan_rows_vt, r};
}

static const DriverStmtVT fdb_scan_stmt_vt = {
    .close = fdb_stmt_close,
    .num_input = fdb_stmt_num_input,
    .exec_context = fdb_stmt_exec_context,
    .query_context = fdb_scan_stmt_query_context,
    .column_converter = fdb_stmt_column_converter,
};

static DriverStmt fdb_scan_conn_prepare_context(void *self, Context ctx, Str query,
                                                Error *err) {
    FakeStmt *s = fdb_conn_prepare(self, ctx, query, err);
    if (s == NULL)
        return (DriverStmt){NULL, NULL};
    return (DriverStmt){&fdb_scan_stmt_vt, s};
}

static const DriverConnVT fdb_scan_conn_vt = {
    .close = fdb_conn_close,
    .begin = fdb_conn_begin,
    .prepare_context = fdb_scan_conn_prepare_context,
    .exec_context = fdb_conn_exec_context,
    .query_context = fdb_conn_query_context,
    .reset_session = fdb_conn_reset_session,
    .is_valid = fdb_conn_is_valid,
};

static DriverConn fdb_connector_connect(void *self, Context ctx, Error *err) {
    (void)ctx;
    FakeConnector *fc = self;
    FakeConn *c = fdb_open(fc->name, err);
    if (c == NULL)
        return (DriverConn){NULL, NULL};
    c->waiter = fc->waiter;
    if (fc->kind == FAKE_BASIC) {
        c->skip_dirty_session = true; /* Conn won't implement ResetSession */
        return (DriverConn){&fdb_basic_conn_vt, c};
    }
    if (fc->kind == FAKE_SCANCOLS)
        return (DriverConn){&fdb_scan_conn_vt, c};
    return (DriverConn){&fdb_conn_vt, c};
}

static Driver fdb_connector_driver(void *self) {
    (void)self;
    return fdriver;
}

static Error fdb_connector_close(void *self) {
    FakeConnector *fc = self;
    if (fc->closed)
        return fmt_errorf_v("fakedb: connector is closed");
    fc->closed = true;
    return BURROW_NO_ERROR;
}

static const DriverConnectorVT fdb_connector_vt = {
    NULL, fdb_connector_connect, fdb_connector_driver, fdb_connector_close};

static DriverConnector fdb_connector(FakeConnector *fc) {
    return (DriverConnector){&fdb_connector_vt, fc};
}

BURROW_UNUSED static FakeCursor *fdb_get_rows_cursor(SqlRows *rs) {
    DriverRows r = burrow__sql_rows_driver(rs);
    if (r.vt == &fdb_scan_rows_vt)
        return ((FakeScanRows *)r.data)->rc;
    return r.data;
}

#endif /* DATABASE_SQL_FAKEDB_H */
