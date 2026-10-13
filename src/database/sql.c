/* Derived from Go's src/database/sql/sql.go, closemu.go and ctxutil.go: the
 * pool, Conn, Tx, Stmt, Rows and Row.
 * Go source: go1.27.1.
 *
 * Go leans on its collector here more than almost anywhere else. A Stmt keeps
 * the driver statements it prepared on each connection, a Rows keeps the
 * statement it came from, and a closed connection lingers for as long as a
 * statement still points at it. Each of those objects carries a reference
 * count instead, and is freed when the last holder lets go: the program's free
 * call, a goroutine finishing, or the object that pointed at it going away.
 * Go's dependency map, which decides when a statement or a connection is
 * really closed, is a count of dependents on each one, with the same panics
 * when it goes wrong.
 *
 * Errors that cross goroutines, a failed connect handed to a waiter or the
 * error that ended a Rows, are copied into an arena the receiving object owns,
 * since the goroutine that made them releases its own.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/database/sql.h"

#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/database/sql/driver.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/proc.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include "sql_internal.h"

#include <stddef.h>
#include <string.h>

/* ----------------------------------------------------------------- errors */

BURROW_SENTINEL_ERROR(sql_err_no_rows, "sql: no rows in result set");
BURROW_SENTINEL_ERROR(sql_err_conn_done, "sql: connection is already closed");
BURROW_SENTINEL_ERROR(sql_err_tx_done,
                      "sql: transaction has already been committed or rolled back");
BURROW_SENTINEL_ERROR(burrow__sql_err_db_closed, "sql: database is closed");
BURROW_SENTINEL_ERROR(burrow__sql_err_rows_closed, "sql: Rows are closed");

/* Go compares ErrSkip and io.EOF with ==, which is the same error value. */
static bool sq_same(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

static bool sq_bad_conn(Error e) {
    return errors_is(e, driver_err_bad_conn);
}

/* A select on ctx.Done() with a default. */
static bool sq_ctx_done(Context ctx) {
    Chan *d = context_done(ctx);
    bool ok = false;
    return d != NULL && chan_try_recv(d, NULL, &ok);
}

static Error sq_new_error(Str msg) {
    return errors_new(error_allocator(), msg);
}

/* A growing array of pointers on the heap. */
typedef struct SqVec {
    void **p;
    Int len;
    Int cap;
} SqVec;

static bool sq_vec_push(SqVec *v, void *x) {
    if (v->len == v->cap) {
        Int nc = v->cap < 4 ? 4 : v->cap * 2;
        void **np = (void **)mem_realloc(heap_allocator(), (void *)v->p,
                                         (size_t)v->cap * sizeof(void *),
                                         (size_t)nc * sizeof(void *), _Alignof(void *));
        if (np == NULL)
            return false;
        v->p = np;
        v->cap = nc;
    }
    v->p[v->len++] = x;
    return true;
}

static void sq_vec_free(SqVec *v) {
    if (v->p != NULL)
        mem_free(heap_allocator(), (void *)v->p, (size_t)v->cap * sizeof(void *),
                 _Alignof(void *));
    v->p = NULL;
    v->len = 0;
    v->cap = 0;
}

/* Hooks for the tests, which Go keeps as package variables of the same names:
 * hookTxGrabConn, rollbackHook, bypassRowsAwaitDone and rowsCloseHook. Only
 * tests set them, while nothing else is running. */
burrow__SqlHook burrow__sql_hook_tx_grab_conn;
burrow__SqlHook burrow__sql_rollback_hook;
bool burrow__sql_bypass_rows_await_done;
burrow__SqlRowsCloseHook burrow__sql_rows_close_hook;

/* --------------------------------------------------------- closingMutex */

/* closemu.go's closingMutex: a read-write lock where a waiting writer keeps
 * new readers out, so that Close is not starved by a stream of calls. state is
 * -1 when it is held for writing, and otherwise twice the readers, plus one
 * when a writer is waiting. */
typedef struct SqCloseMu {
    SyncAtomicInt64 state;
    SyncMutex mu;
    SyncCond read;
    SyncCond write;
    bool inited;
} SqCloseMu;

static void sq_closemu_init(SqCloseMu *m) {
    if (!m->inited) {
        m->read = SYNC_COND(sync_mutex_locker(&m->mu));
        m->write = SYNC_COND(sync_mutex_locker(&m->mu));
        m->inited = true;
    }
}

static bool sq_closemu_try_rlock(SqCloseMu *m) {
    for (;;) {
        int64_t x = sync_atomic_int64_load(&m->state);
        if (x < 0 || x == 1)
            return false;
        if (sync_atomic_int64_compare_and_swap(&m->state, x, x + 2))
            return true;
    }
}

static void sq_closemu_rlock(SqCloseMu *m) {
    if (sq_closemu_try_rlock(m))
        return;
    sync_mutex_lock(&m->mu);
    for (;;) {
        if (sq_closemu_try_rlock(m))
            break;
        sq_closemu_init(m);
        sync_cond_wait(&m->read);
    }
    sync_mutex_unlock(&m->mu);
}

static void sq_closemu_runlock(SqCloseMu *m) {
    for (;;) {
        int64_t x = sync_atomic_int64_load(&m->state);
        if (x < 2)
            panic_str(BURROW_S("runlock of un-rlocked mutex"));
        if (sync_atomic_int64_compare_and_swap(&m->state, x, x - 2)) {
            if (x - 2 == 1) {
                sync_mutex_lock(&m->mu);
                sq_closemu_init(m);
                sync_cond_broadcast(&m->write);
                sync_mutex_unlock(&m->mu);
            }
            return;
        }
    }
}

static void sq_closemu_lock(SqCloseMu *m) {
    sync_mutex_lock(&m->mu);
    for (;;) {
        int64_t x = sync_atomic_int64_load(&m->state);
        if (x == 0 || x == 1) {
            if (sync_atomic_int64_compare_and_swap(&m->state, x, -1))
                break;
            continue;
        }
        if ((x & 1) == 0 && !sync_atomic_int64_compare_and_swap(&m->state, x, x | 1))
            continue;
        sq_closemu_init(m);
        sync_cond_wait(&m->write);
    }
    sync_mutex_unlock(&m->mu);
}

static void sq_closemu_unlock(SqCloseMu *m) {
    sync_mutex_lock(&m->mu);
    if (!sync_atomic_int64_compare_and_swap(&m->state, -1, 0)) {
        sync_mutex_unlock(&m->mu);
        panic_str(BURROW_S("unlock of unlocked mutex"));
    }
    if (m->inited) {
        sync_cond_broadcast(&m->read);
        sync_cond_broadcast(&m->write);
    }
    sync_mutex_unlock(&m->mu);
}

/* ------------------------------------------------------------------ types */

typedef struct SqDC SqDC;
typedef struct SqDS SqDS;

/* driverStmt: a statement the driver prepared on one connection. Held by the
 * connection's list of open statements, by each Stmt that uses it and by the
 * work queued to close it, and it holds its connection. */
struct SqDS {
    SqDC *dc;
    DriverStmt si;
    bool closed; /* dc->mu */
    /* dc->open_stmt, under dc->mu. */
    SqDS *os_prev;
    SqDS *os_next;
    bool in_open;
    /* dc->on_put, under db->mu. */
    SqDS *put_next;
    SyncAtomicInt64 refs;
};

/* What DB.Close is to do with a connection, decided under db->mu and done
 * after it, which is Go's closeDBLocked returning a func. */
typedef enum SqCloseResult {
    SQ_CLOSE_NONE,
    SQ_CLOSE_FINAL,
    SQ_CLOSE_DUP,
} SqCloseResult;

/* driverConn. Alive until its final close, after which only the statements
 * that still point at it keep the struct. */
struct SqDC {
    SqlDB *db;
    Time created_at;

    SyncMutex mu; /* guards the following */
    DriverConn ci;
    bool need_reset;
    bool closed;
    bool final_closed;
    SqDS *open_stmt;

    /* Guarded by db->mu. */
    bool in_use;
    bool dbmu_closed;
    Time returned_at;
    SqDS *on_put;
    SqCloseResult close_result;
    /* A list of connections to close once db->mu is let go. */
    SqDC *close_next;

    SyncAtomicInt64 refs;
};

/* A goroutine in conn waiting for a connection. The pool hands it one, or
 * the error from opening one, under db->mu, and the waiter owns the struct. */
typedef struct SqWaiter {
    Chan *ch;
    SqDC *conn;
    Error err;
    Arena err_arena;
    Int idx;
} SqWaiter;

/* Open's dsnConnector, for a driver without open_connector. */
typedef struct SqDsn {
    Str dsn;
    Driver driver;
} SqDsn;

struct SqlDB {
    SyncAtomicInt64 wait_duration;
    DriverConnector connector;
    SqDsn *dsn;
    SyncAtomicInt64 num_closed;

    SyncMutex mu; /* guards the following */
    SqVec free_conn;
    SqVec reqs;
    Int num_open;
    Int num_dep; /* len(Go's db.dep): open conns and Stmts with dependents */
    Int opener_pending;
    SyncCond opener_cond;
    bool opener_stop;
    bool closed;
    Int max_idle_count;
    Int max_open;
    Duration max_lifetime;
    Duration max_idle_time;
    Chan *cleaner_ch;
    Duration cleaner_start;
    int64_t wait_count;
    int64_t max_idle_closed;
    int64_t max_idle_time_closed;
    int64_t max_lifetime_closed;

    Context opener_ctx;
    ContextCancelFunc opener_cancel;

    SyncAtomicInt64 refs;
};

/* What a call does with its connection when it is done with it, Go's
 * releaseConn func. */
typedef enum SqReleaseKind {
    SQ_RELEASE_NONE,
    SQ_RELEASE_DC,
    SQ_RELEASE_CONN,
    SQ_RELEASE_TX,
} SqReleaseKind;

typedef struct SqRelease {
    SqReleaseKind kind;
    void *obj;
} SqRelease;

struct SqlConn {
    SqlDB *db;
    SqCloseMu closemu;
    SqDC *dc;
    SyncAtomicBool done;
    SyncAtomicInt64 refs;
};

struct SqlTx {
    SqlDB *db;
    SqCloseMu closemu;
    SqDC *dc;
    DriverTx txi;
    SqRelease release;
    SyncAtomicBool done;
    bool keep_conn_on_rollback;
    SyncMutex stmts_mu;
    SqlStmt *stmts;
    SqlStmt *stmts_last;
    Context ctx;
    ContextCancelFunc cancel;
    SyncAtomicInt64 refs;
};

typedef enum SqGrabber {
    SQ_CG_NONE,
    SQ_CG_CONN,
    SQ_CG_TX,
} SqGrabber;

struct SqlStmt {
    SqlDB *db;
    Str query;
    Error sticky;
    Arena sticky_arena;

    SqCloseMu closemu;

    SqGrabber cg_kind;
    void *cg;
    SqDS *cgds;
    SqlStmt *parent;

    SyncMutex mu; /* guards closed and css */
    bool closed;
    SqVec css;
    int64_t last_num_closed;

    int64_t deps; /* db->mu */
    SqlStmt *tx_next;
    SyncAtomicInt64 refs;
};

struct SqlRows {
    SqDC *dc;
    SqRelease release;
    SqlStmt *dep_stmt;
    DriverRows rowsi;
    bool has_ctx;
    Context closectx;
    ContextCancelFunc cancel;
    SqlTx *await_tx;
    SqlRows *children;
    SqlRows *child_next;
    SqDS *close_stmt;

    SyncAtomicBool has_cd;
    Error context_done;
    SyncAtomicBool closing;

    SqCloseMu closemu;
    Error lasterr;
    bool closed;
    bool scan_hold;
    bool hit_eof;
    bool next_called;
    DriverValue *lastcols;
    Int lastcols_len;
    Int numcols;

    Bytes raw;
    Arena raw_arena;
    Alloc *scan_alloc; /* what the Scan under way was given */
    Arena err_arena;
    SyncAtomicInt64 refs;
};

struct SqlColumnType {
    Str name;
    bool has_nullable;
    bool has_length;
    bool has_precision_scale;
    bool nullable;
    int64_t length;
    Str database_type;
    int64_t precision;
    int64_t scale;
    const Type *scan_type;
};

const Type burrow__sql_rows_type = {
    {(const Byte *)"Rows", 4},
    {(const Byte *)"database/sql", 12},
    KIND_STRUCT,
    (uint32_t)sizeof(struct SqlRows),
    (uint16_t)_Alignof(struct SqlRows),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x73717277U, /* "sqrw" */
    NULL,
};

const Type burrow_type_SqlRowsPtr = {
    {NULL, 0},
    {NULL, 0},
    KIND_POINTER,
    (uint32_t)sizeof(void *),
    (uint16_t)_Alignof(void *),
    0,
    0,
    NULL,
    NULL,
    &burrow__sql_rows_type,
    NULL,
    0,
    0x73717270U, /* "sqrp" */
    NULL,
};

static const Type sq_dsn_type = {
    {(const Byte *)"dsnConnector", 12},
    {(const Byte *)"database/sql", 12},
    KIND_STRUCT,
    (uint32_t)sizeof(SqDsn),
    (uint16_t)_Alignof(SqDsn),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x73716463U, /* "sqdc" */
    NULL,
};

/* --------------------------------------------------------- reference counts */

static void sq_maybe_open_locked(SqlDB *db);
static void sq_start_cleaner_locked(SqlDB *db);
static void sq_put_conn(SqDC *dc, Error err, bool reset_session);
static Error sq_stmt_final_close(SqlStmt *s);
static Error sq_conn_close(SqlConn *c, Error err);
static void sq_conn_unref(SqlConn *c);
static void sq_tx_unref(SqlTx *tx);
static void sq_stmt_unref(SqlStmt *s);
static void sq_rows_unref(SqlRows *rs);

static void sq_db_ref(SqlDB *db) {
    sync_atomic_int64_add(&db->refs, 1);
}

static void sq_db_unref(SqlDB *db) {
    if (sync_atomic_int64_add(&db->refs, -1) != 0)
        return;
    sq_vec_free(&db->free_conn);
    sq_vec_free(&db->reqs);
    context_release(db->opener_ctx);
    if (db->dsn != NULL) {
        if (db->dsn->dsn.len > 0)
            mem_free(heap_allocator(), (void *)(uintptr_t)db->dsn->dsn.p,
                     (size_t)db->dsn->dsn.len, 1);
        mem_free(heap_allocator(), db->dsn, sizeof(SqDsn), _Alignof(SqDsn));
    }
    mem_free(heap_allocator(), db, sizeof(SqlDB), _Alignof(SqlDB));
}

static void sq_dc_unref(SqDC *dc) {
    if (sync_atomic_int64_add(&dc->refs, -1) != 0)
        return;
    SqlDB *db = dc->db;
    mem_free(heap_allocator(), dc, sizeof(SqDC), _Alignof(SqDC));
    sq_db_unref(db);
}

static void sq_ds_ref(SqDS *ds) {
    sync_atomic_int64_add(&ds->refs, 1);
}

static void sq_ds_unref(SqDS *ds) {
    if (sync_atomic_int64_add(&ds->refs, -1) != 0)
        return;
    SqDC *dc = ds->dc;
    mem_free(heap_allocator(), ds, sizeof(SqDS), _Alignof(SqDS));
    sq_dc_unref(dc);
}

/* ------------------------------------------------------------ dsnConnector */

static DriverConn sq_dsn_connect(void *self, Context ctx, Error *err) {
    (void)ctx;
    SqDsn *t = (SqDsn *)self;
    return t->driver.vt->open(t->driver.data, t->dsn, err);
}

static Driver sq_dsn_driver(void *self) {
    return ((SqDsn *)self)->driver;
}

static const DriverConnectorVT sq_dsn_vt = {&sq_dsn_type, sq_dsn_connect, sq_dsn_driver,
                                            NULL};

/* ----------------------------------------------------- driverConn and Stmt */

static SqDC *sq_dc_new(SqlDB *db, DriverConn ci) {
    SqDC *dc = BURROW_NEW(heap_allocator(), SqDC);
    if (dc == NULL)
        return NULL;
    dc->db = db;
    dc->ci = ci;
    dc->created_at = time_now();
    dc->returned_at = dc->created_at;
    sync_atomic_int64_store(&dc->refs, 1);
    sq_db_ref(db);
    db->num_dep++; /* the conn depends on itself until it closes */
    return dc;
}

static SqDS *sq_ds_new(SqDC *dc, DriverStmt si) {
    SqDS *ds = BURROW_NEW(heap_allocator(), SqDS);
    if (ds == NULL)
        return NULL;
    ds->dc = dc;
    ds->si = si;
    sync_atomic_int64_store(&ds->refs, 1);
    sync_atomic_int64_add(&dc->refs, 1);
    return ds;
}

/* driverStmt.Close. A second close returns nil rather than the first one's
 * error, which nothing in Go reads. */
static Error sq_ds_close(SqDS *ds) {
    Error err = BURROW_NO_ERROR;
    sync_mutex_lock(&ds->dc->mu);
    if (!ds->closed) {
        ds->closed = true;
        err = ds->si.vt->close(ds->si.data);
    }
    sync_mutex_unlock(&ds->dc->mu);
    return err;
}

static void sq_dc_unlink_locked(SqDC *dc, SqDS *ds) {
    if (ds->os_prev != NULL)
        ds->os_prev->os_next = ds->os_next;
    else
        dc->open_stmt = ds->os_next;
    if (ds->os_next != NULL)
        ds->os_next->os_prev = ds->os_prev;
    ds->os_prev = NULL;
    ds->os_next = NULL;
    ds->in_open = false;
}

static void sq_dc_remove_open_stmt(SqDC *dc, SqDS *ds) {
    bool was = false;
    sync_mutex_lock(&dc->mu);
    if (ds->in_open) {
        sq_dc_unlink_locked(dc, ds);
        was = true;
    }
    sync_mutex_unlock(&dc->mu);
    if (was)
        sq_ds_unref(ds);
}

static bool sq_dc_expired(const SqDC *dc, Duration timeout) {
    if (timeout <= 0)
        return false;
    return time_before(time_add(dc->created_at, timeout), time_now());
}

static Error sq_dc_reset_session(SqDC *dc, Context ctx) {
    Error err = BURROW_NO_ERROR;
    sync_mutex_lock(&dc->mu);
    if (dc->need_reset && dc->ci.vt->reset_session != NULL)
        err = dc->ci.vt->reset_session(dc->ci.data, ctx);
    sync_mutex_unlock(&dc->mu);
    return err;
}

static bool sq_dc_validate(SqDC *dc, bool needs_reset) {
    bool ok = true;
    sync_mutex_lock(&dc->mu);
    if (needs_reset)
        dc->need_reset = true;
    if (dc->ci.vt->is_valid != NULL)
        ok = dc->ci.vt->is_valid(dc->ci.data);
    sync_mutex_unlock(&dc->mu);
    return ok;
}

static DriverStmt sq_ctx_driver_prepare(Context ctx, DriverConn ci, Str query,
                                        Error *err);

/* prepareLocked. The statement goes on the connection's open list only when
 * it belongs to a DB Stmt, which is what closes it otherwise. Returns a
 * reference for the caller. */
static SqDS *sq_dc_prepare_locked(SqDC *dc, Context ctx, bool has_cg, Str query,
                                  Error *err) {
    Error e = BURROW_NO_ERROR;
    DriverStmt si = sq_ctx_driver_prepare(ctx, dc->ci, query, &e);
    if (BURROW_FAILED(e)) {
        *err = e;
        return NULL;
    }
    SqDS *ds = sq_ds_new(dc, si);
    if (ds == NULL) {
        si.vt->close(si.data);
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    if (!has_cg) {
        ds->os_next = dc->open_stmt;
        if (dc->open_stmt != NULL)
            dc->open_stmt->os_prev = ds;
        dc->open_stmt = ds;
        ds->in_open = true;
        sq_ds_ref(ds);
    }
    *err = BURROW_NO_ERROR;
    return ds;
}

static Error sq_dc_final_close(SqDC *dc) {
    SqlDB *db = dc->db;
    sync_mutex_lock(&dc->mu);
    SqDS *open = dc->open_stmt;
    dc->open_stmt = NULL;
    for (SqDS *ds = open; ds != NULL; ds = ds->os_next)
        ds->in_open = false;
    sync_mutex_unlock(&dc->mu);
    while (open != NULL) {
        SqDS *next = open->os_next;
        open->os_prev = NULL;
        open->os_next = NULL;
        (void)sq_ds_close(open);
        sq_ds_unref(open);
        open = next;
    }

    sync_mutex_lock(&dc->mu);
    dc->final_closed = true;
    Error err = dc->ci.vt->close(dc->ci.data);
    dc->ci = (DriverConn){NULL, NULL};
    sync_mutex_unlock(&dc->mu);

    sync_mutex_lock(&db->mu);
    db->num_open--;
    sq_maybe_open_locked(db);
    sync_mutex_unlock(&db->mu);

    sync_atomic_int64_add(&db->num_closed, 1);
    sq_dc_unref(dc);
    return err;
}

/* driverConn.Close. The connection's only dependency is itself, so removing
 * it is always the last one. */
static Error sq_dc_close(SqDC *dc) {
    sync_mutex_lock(&dc->mu);
    if (dc->closed) {
        sync_mutex_unlock(&dc->mu);
        return sq_new_error(BURROW_S("sql: duplicate driverConn close"));
    }
    dc->closed = true;
    sync_mutex_unlock(&dc->mu);

    sync_mutex_lock(&dc->db->mu);
    dc->dbmu_closed = true;
    dc->db->num_dep--;
    sync_mutex_unlock(&dc->db->mu);
    return sq_dc_final_close(dc);
}

/* closeDBLocked. What to do is kept in close_result for after db->mu. */
static void sq_dc_close_db_locked(SqDC *dc) {
    sync_mutex_lock(&dc->mu);
    if (dc->closed) {
        dc->close_result = SQ_CLOSE_DUP;
    } else {
        dc->closed = true;
        dc->close_result = SQ_CLOSE_FINAL;
        dc->db->num_dep--;
    }
    sync_mutex_unlock(&dc->mu);
}

static Error sq_dc_run_close_result(SqDC *dc) {
    SqCloseResult r = dc->close_result;
    dc->close_result = SQ_CLOSE_NONE;
    if (r == SQ_CLOSE_FINAL)
        return sq_dc_final_close(dc);
    if (r == SQ_CLOSE_DUP)
        return sq_new_error(BURROW_S("sql: duplicate driverConn close"));
    return BURROW_NO_ERROR;
}

/* addDep and removeDep for a Stmt, whose dependents are itself, its Rows and
 * the Stmts a Tx made from it. */
static void sq_stmt_add_dep(SqlStmt *s) {
    sync_mutex_lock(&s->db->mu);
    if (s->deps++ == 0)
        s->db->num_dep++;
    sync_mutex_unlock(&s->db->mu);
}

static Error sq_stmt_remove_dep(SqlStmt *s) {
    sync_mutex_lock(&s->db->mu);
    if (s->deps <= 0) {
        sync_mutex_unlock(&s->db->mu);
        panic_str(BURROW_S("unpaired removeDep: no deps for *sql.Stmt"));
    }
    s->deps--;
    bool last = s->deps == 0;
    if (last)
        s->db->num_dep--;
    sync_mutex_unlock(&s->db->mu);
    if (last)
        return sq_stmt_final_close(s);
    return BURROW_NO_ERROR;
}

/* ---------------------------------------------------------------- waiters */

static SqWaiter *sq_waiter_new(void) {
    SqWaiter *w = BURROW_NEW(heap_allocator(), SqWaiter);
    if (w == NULL)
        return NULL;
    w->ch = chan_make(heap_allocator(), TYPE_BOOL, 1);
    if (w->ch == NULL) {
        mem_free(heap_allocator(), w, sizeof(SqWaiter), _Alignof(SqWaiter));
        return NULL;
    }
    arena_init(&w->err_arena, heap_allocator(), 0);
    w->idx = -1;
    return w;
}

static void sq_waiter_free(SqWaiter *w) {
    chan_free(w->ch);
    arena_free(&w->err_arena);
    mem_free(heap_allocator(), w, sizeof(SqWaiter), _Alignof(SqWaiter));
}

static bool sq_reqs_add(SqlDB *db, SqWaiter *w) {
    w->idx = db->reqs.len;
    if (!sq_vec_push(&db->reqs, w)) {
        w->idx = -1;
        return false;
    }
    return true;
}

static bool sq_reqs_delete(SqlDB *db, SqWaiter *w) {
    Int i = w->idx;
    if (i < 0)
        return false;
    Int last = db->reqs.len - 1;
    if (i != last) {
        SqWaiter *moved = (SqWaiter *)db->reqs.p[last];
        db->reqs.p[i] = moved;
        moved->idx = i;
    }
    db->reqs.p[last] = NULL;
    db->reqs.len = last;
    w->idx = -1;
    return true;
}

static SqWaiter *sq_reqs_take_random(SqlDB *db) {
    if (db->reqs.len == 0)
        return NULL;
    Int i = (Int)(runtime_rand64() % (uint64_t)db->reqs.len);
    SqWaiter *w = (SqWaiter *)db->reqs.p[i];
    (void)sq_reqs_delete(db, w);
    return w;
}

static void sq_reqs_close_all(SqlDB *db) {
    for (Int i = 0; i < db->reqs.len; i++) {
        SqWaiter *w = (SqWaiter *)db->reqs.p[i];
        w->idx = -1;
        chan_close(w->ch);
    }
    db->reqs.len = 0;
}

/* --------------------------------------------------------------- the pool */

enum {
    SQ_ALWAYS_NEW = 0,
    SQ_CACHED_OR_NEW = 1,
};

static Int sq_max_idle_conns_locked(const SqlDB *db) {
    Int n = db->max_idle_count;
    if (n == 0)
        return 2;
    if (n < 0)
        return 0;
    return n;
}

static Duration sq_shortest_idle_time_locked(const SqlDB *db) {
    if (db->max_idle_time <= 0)
        return db->max_lifetime;
    if (db->max_lifetime <= 0)
        return db->max_idle_time;
    return db->max_idle_time < db->max_lifetime ? db->max_idle_time : db->max_lifetime;
}

/* putConnDBLocked. Hands dc, or the error from opening a connection, to a
 * waiter, or keeps dc idle. False means the caller has to close dc. */
static bool sq_put_conn_db_locked(SqlDB *db, SqDC *dc, Error err) {
    if (db->closed)
        return false;
    if (db->max_open > 0 && db->num_open > db->max_open)
        return false;
    SqWaiter *w = sq_reqs_take_random(db);
    if (w != NULL) {
        if (!BURROW_FAILED(err))
            dc->in_use = true;
        w->conn = dc;
        w->err = BURROW_FAILED(err) ? error_retain(arena_allocator(&w->err_arena), err)
                                    : BURROW_NO_ERROR;
        bool v = true;
        (void)chan_try_send(w->ch, &v);
        return true;
    }
    if (!BURROW_FAILED(err) && !db->closed) {
        if (sq_max_idle_conns_locked(db) > db->free_conn.len) {
            if (!sq_vec_push(&db->free_conn, dc))
                return false;
            sq_start_cleaner_locked(db);
            return true;
        }
        db->max_idle_closed++;
    }
    return false;
}

/* maybeOpenNewConnections. The opener is told through a count and a
 * condition rather than Go's buffered channel. */
static void sq_maybe_open_locked(SqlDB *db) {
    Int n = db->reqs.len;
    if (db->max_open > 0) {
        Int can = db->max_open - db->num_open;
        if (n > can)
            n = can;
    }
    while (n > 0) {
        db->num_open++;
        n--;
        if (db->closed)
            return;
        db->opener_pending++;
        sync_cond_signal(&db->opener_cond);
    }
}

static void sq_open_new_connection(SqlDB *db) {
    Error err = BURROW_NO_ERROR;
    DriverConn ci = db->connector.vt->connect(db->connector.data, db->opener_ctx, &err);
    sync_mutex_lock(&db->mu);
    if (db->closed) {
        if (!BURROW_FAILED(err))
            ci.vt->close(ci.data);
        db->num_open--;
        sync_mutex_unlock(&db->mu);
        return;
    }
    if (BURROW_FAILED(err)) {
        db->num_open--;
        (void)sq_put_conn_db_locked(db, NULL, err);
        sq_maybe_open_locked(db);
        sync_mutex_unlock(&db->mu);
        return;
    }
    SqDC *dc = sq_dc_new(db, ci);
    if (dc == NULL) {
        db->num_open--;
        ci.vt->close(ci.data);
        (void)sq_put_conn_db_locked(db, NULL, burrow_err_out_of_memory);
        sync_mutex_unlock(&db->mu);
        return;
    }
    if (sq_put_conn_db_locked(db, dc, BURROW_NO_ERROR)) {
        sync_mutex_unlock(&db->mu);
        return;
    }
    db->num_open--;
    db->num_dep--;
    ci.vt->close(ci.data);
    sync_mutex_unlock(&db->mu);
    sq_dc_unref(dc);
}

/* connectionOpener. */
static void sq_opener(void *env) {
    SqlDB *db = (SqlDB *)env;
    sync_mutex_lock(&db->mu);
    for (;;) {
        while (db->opener_pending == 0 && !db->opener_stop)
            sync_cond_wait(&db->opener_cond);
        if (db->opener_stop)
            break;
        db->opener_pending--;
        sync_mutex_unlock(&db->mu);
        ArenaMark m = error_mark();
        sq_open_new_connection(db);
        error_release(m);
        sync_mutex_lock(&db->mu);
    }
    sync_mutex_unlock(&db->mu);
    sq_db_unref(db);
}

/* conn. A connection, idle or new, or one handed over by putConn when the
 * pool is at its limit. */
static SqDC *sq_conn(SqlDB *db, Context ctx, int strategy, Error *err) {
    sync_mutex_lock(&db->mu);
    if (db->closed) {
        sync_mutex_unlock(&db->mu);
        *err = burrow__sql_err_db_closed;
        return NULL;
    }
    if (sq_ctx_done(ctx)) {
        sync_mutex_unlock(&db->mu);
        *err = context_err(ctx);
        return NULL;
    }
    Duration lifetime = db->max_lifetime;

    Int last = db->free_conn.len - 1;
    if (strategy == SQ_CACHED_OR_NEW && last >= 0) {
        SqDC *c = (SqDC *)db->free_conn.p[last];
        db->free_conn.p[last] = NULL;
        db->free_conn.len = last;
        c->in_use = true;
        if (sq_dc_expired(c, lifetime)) {
            db->max_lifetime_closed++;
            sync_mutex_unlock(&db->mu);
            (void)sq_dc_close(c);
            *err = driver_err_bad_conn;
            return NULL;
        }
        sync_mutex_unlock(&db->mu);
        Error e = sq_dc_reset_session(c, ctx);
        if (sq_bad_conn(e)) {
            (void)sq_dc_close(c);
            *err = e;
            return NULL;
        }
        *err = BURROW_NO_ERROR;
        return c;
    }

    if (db->max_open > 0 && db->num_open >= db->max_open) {
        SqWaiter *w = sq_waiter_new();
        if (w == NULL || !sq_reqs_add(db, w)) {
            sync_mutex_unlock(&db->mu);
            if (w != NULL)
                sq_waiter_free(w);
            *err = burrow_err_out_of_memory;
            return NULL;
        }
        db->wait_count++;
        sync_mutex_unlock(&db->mu);

        Time wait_start = time_now();
        bool v = false;
        bool ok = false;
        SelectCase cases[2] = {BURROW_RECV(context_done(ctx), NULL),
                               BURROW_RECV_OK(w->ch, &v, &ok)};
        Int which = chan_select(cases, 2);
        if (which == 0) {
            sync_mutex_lock(&db->mu);
            bool deleted = sq_reqs_delete(db, w);
            sync_mutex_unlock(&db->mu);
            sync_atomic_int64_add(&db->wait_duration, (int64_t)time_since(wait_start));
            if (!deleted) {
                bool rok = false;
                if (chan_try_recv(w->ch, &v, &rok) && rok && w->conn != NULL)
                    sq_put_conn(w->conn, w->err, false);
            }
            sq_waiter_free(w);
            *err = context_err(ctx);
            return NULL;
        }
        sync_atomic_int64_add(&db->wait_duration, (int64_t)time_since(wait_start));
        if (!ok) {
            sq_waiter_free(w);
            *err = burrow__sql_err_db_closed;
            return NULL;
        }
        SqDC *c = w->conn;
        Error e = BURROW_FAILED(w->err) ? error_retain(error_allocator(), w->err)
                                        : BURROW_NO_ERROR;
        sq_waiter_free(w);
        if (strategy == SQ_CACHED_OR_NEW && !BURROW_FAILED(e) &&
            sq_dc_expired(c, lifetime)) {
            sync_mutex_lock(&db->mu);
            db->max_lifetime_closed++;
            sync_mutex_unlock(&db->mu);
            (void)sq_dc_close(c);
            *err = driver_err_bad_conn;
            return NULL;
        }
        if (c == NULL) {
            *err = e;
            return NULL;
        }
        Error re = sq_dc_reset_session(c, ctx);
        if (sq_bad_conn(re)) {
            (void)sq_dc_close(c);
            *err = re;
            return NULL;
        }
        *err = e;
        return c;
    }

    db->num_open++; /* optimistically */
    sync_mutex_unlock(&db->mu);
    Error ce = BURROW_NO_ERROR;
    DriverConn ci = db->connector.vt->connect(db->connector.data, ctx, &ce);
    if (BURROW_FAILED(ce)) {
        sync_mutex_lock(&db->mu);
        db->num_open--; /* correct for earlier optimism */
        sq_maybe_open_locked(db);
        sync_mutex_unlock(&db->mu);
        *err = ce;
        return NULL;
    }
    sync_mutex_lock(&db->mu);
    SqDC *dc = sq_dc_new(db, ci);
    if (dc == NULL) {
        db->num_open--;
        sq_maybe_open_locked(db);
        sync_mutex_unlock(&db->mu);
        ci.vt->close(ci.data);
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    dc->in_use = true;
    sync_mutex_unlock(&db->mu);
    *err = BURROW_NO_ERROR;
    return dc;
}

/* putConn. Gives dc back to the pool, or closes it when err says it is bad. */
static void sq_put_conn(SqDC *dc, Error err, bool reset_session) {
    SqlDB *db = dc->db;
    if (!sq_bad_conn(err)) {
        if (!sq_dc_validate(dc, reset_session))
            err = driver_err_bad_conn;
    }
    sync_mutex_lock(&db->mu);
    if (!dc->in_use) {
        sync_mutex_unlock(&db->mu);
        panic_str(BURROW_S("sql: connection returned that was never out"));
    }
    if (!sq_bad_conn(err) && sq_dc_expired(dc, db->max_lifetime)) {
        db->max_lifetime_closed++;
        err = driver_err_bad_conn;
    }
    dc->in_use = false;
    dc->returned_at = time_now();

    SqDS *put = dc->on_put;
    dc->on_put = NULL;
    while (put != NULL) {
        SqDS *next = put->put_next;
        put->put_next = NULL;
        (void)sq_ds_close(put);
        sq_ds_unref(put);
        put = next;
    }

    if (sq_bad_conn(err)) {
        /* Don't reuse bad connections. finalClose takes care of the open
         * count. */
        sq_maybe_open_locked(db);
        sync_mutex_unlock(&db->mu);
        (void)sq_dc_close(dc);
        return;
    }
    bool added = sq_put_conn_db_locked(db, dc, BURROW_NO_ERROR);
    sync_mutex_unlock(&db->mu);
    if (!added)
        (void)sq_dc_close(dc);
}

/* connectionCleanerRunLocked. Takes the connections that have been idle or
 * open too long out of the free list, as a list through close_next, and
 * answers how long until the next one will be. */
static Duration sq_cleaner_run_locked(SqlDB *db, Duration d, SqDC **closing) {
    SqDC *head = NULL;
    SqDC **tail = &head;
    int64_t idle_closing = 0;
    int64_t n_closing = 0;
    if (db->max_idle_time > 0) {
        /* As freeConn is ordered by returnedAt, go through it in reverse to
         * keep the work down. */
        Time idle_since = time_add(time_now(), -db->max_idle_time);
        for (Int i = db->free_conn.len - 1; i >= 0; i--) {
            SqDC *c = (SqDC *)db->free_conn.p[i];
            if (time_before(c->returned_at, idle_since)) {
                i++;
                for (Int j = 0; j < i; j++) {
                    SqDC *x = (SqDC *)db->free_conn.p[j];
                    x->close_next = NULL;
                    *tail = x;
                    tail = &x->close_next;
                }
                memmove((void *)db->free_conn.p, (void *)(db->free_conn.p + i),
                        (size_t)(db->free_conn.len - i) * sizeof(void *));
                db->free_conn.len -= i;
                idle_closing = (int64_t)i;
                n_closing = idle_closing;
                db->max_idle_time_closed += idle_closing;
                break;
            }
        }
        if (db->free_conn.len > 0) {
            SqDC *c = (SqDC *)db->free_conn.p[0];
            Duration d2 = time_sub(c->returned_at, idle_since);
            if (d2 < d)
                d = d2;
        }
    }
    if (db->max_lifetime > 0) {
        Time expired_since = time_add(time_now(), -db->max_lifetime);
        for (Int i = 0; i < db->free_conn.len; i++) {
            SqDC *c = (SqDC *)db->free_conn.p[i];
            if (time_before(c->created_at, expired_since)) {
                c->close_next = NULL;
                *tail = c;
                tail = &c->close_next;
                n_closing++;
                /* A slow delete, since the order is what reuses the least
                 * idle connection first. */
                Int last = db->free_conn.len - 1;
                memmove((void *)(db->free_conn.p + i),
                        (void *)(db->free_conn.p + i + 1),
                        (size_t)(last - i) * sizeof(void *));
                db->free_conn.p[last] = NULL;
                db->free_conn.len = last;
                i--;
            } else {
                Duration d2 = time_sub(c->created_at, expired_since);
                if (d2 < d)
                    d = d2;
            }
        }
        db->max_lifetime_closed += n_closing - idle_closing;
    }
    *closing = head;
    return d;
}

static void sq_close_list(SqDC *c) {
    while (c != NULL) {
        SqDC *next = c->close_next;
        c->close_next = NULL;
        (void)sq_dc_close(c);
        c = next;
    }
}

/* connectionCleaner. */
static void sq_cleaner(void *env) {
    SqlDB *db = (SqlDB *)env;
    const Duration min_interval = TIME_SECOND;

    sync_mutex_lock(&db->mu);
    Duration d = db->cleaner_start;
    Chan *ch = db->cleaner_ch;
    sync_mutex_unlock(&db->mu);
    if (d < min_interval)
        d = min_interval;
    TimeTimer *t = time_new_timer(heap_allocator(), d);
    if (t == NULL) {
        sync_mutex_lock(&db->mu);
        db->cleaner_ch = NULL;
        sync_mutex_unlock(&db->mu);
        chan_free(ch);
        sq_db_unref(db);
        return;
    }

    for (;;) {
        SelectCase cases[2] = {BURROW_RECV(time_timer_c(t), NULL),
                               BURROW_RECV(ch, NULL)};
        (void)chan_select(cases, 2);

        ArenaMark m = error_mark();
        sync_mutex_lock(&db->mu);
        d = sq_shortest_idle_time_locked(db);
        if (db->closed || db->num_open == 0 || d <= 0) {
            db->cleaner_ch = NULL;
            sync_mutex_unlock(&db->mu);
            error_release(m);
            break;
        }
        SqDC *closing = NULL;
        d = sq_cleaner_run_locked(db, d, &closing);
        sync_mutex_unlock(&db->mu);
        sq_close_list(closing);
        error_release(m);

        if (d < min_interval)
            d = min_interval;
        (void)time_timer_reset(t, d, NULL);
    }
    time_timer_free(t);
    chan_free(ch);
    sq_db_unref(db);
}

/* startCleanerLocked. */
static void sq_start_cleaner_locked(SqlDB *db) {
    if ((db->max_lifetime > 0 || db->max_idle_time > 0) && db->num_open > 0 &&
        db->cleaner_ch == NULL) {
        Chan *ch = chan_make(heap_allocator(), TYPE_BOOL, 1);
        if (ch == NULL)
            return;
        db->cleaner_ch = ch;
        db->cleaner_start = sq_shortest_idle_time_locked(db);
        sq_db_ref(db);
        if (!go(BURROW_FN(Func, sq_cleaner, db))) {
            db->cleaner_ch = NULL;
            chan_free(ch);
            sync_atomic_int64_add(&db->refs, -1);
        }
    }
}

/* Wakes the cleaner when a limit changed, if there is one to wake. */
static void sq_poke_cleaner_locked(SqlDB *db) {
    if (db->cleaner_ch != NULL && !db->closed) {
        bool v = true;
        (void)chan_try_send(db->cleaner_ch, &v);
    }
}

/* ------------------------------------------------------------ DB settings */

void sql_db_set_max_idle_conns(SqlDB *db, Int n) {
    sync_mutex_lock(&db->mu);
    if (n > 0)
        db->max_idle_count = n;
    else
        db->max_idle_count = -1; /* No idle connections. */
    /* Make sure maxIdle doesn't exceed maxOpen. */
    if (db->max_open > 0 && sq_max_idle_conns_locked(db) > db->max_open)
        db->max_idle_count = db->max_open;
    SqDC *closing = NULL;
    SqDC **tail = &closing;
    Int idle_count = db->free_conn.len;
    Int max_idle = sq_max_idle_conns_locked(db);
    if (idle_count > max_idle) {
        for (Int i = max_idle; i < idle_count; i++) {
            SqDC *c = (SqDC *)db->free_conn.p[i];
            c->close_next = NULL;
            *tail = c;
            tail = &c->close_next;
            db->free_conn.p[i] = NULL;
        }
        db->free_conn.len = max_idle;
    }
    db->max_idle_closed += (int64_t)(idle_count - db->free_conn.len);
    sync_mutex_unlock(&db->mu);
    sq_close_list(closing);
}

void sql_db_set_max_open_conns(SqlDB *db, Int n) {
    sync_mutex_lock(&db->mu);
    db->max_open = n;
    if (n < 0)
        db->max_open = 0;
    bool sync_max_idle =
        db->max_open > 0 && sq_max_idle_conns_locked(db) > db->max_open;
    sync_mutex_unlock(&db->mu);
    if (sync_max_idle)
        sql_db_set_max_idle_conns(db, n);
}

void sql_db_set_conn_max_lifetime(SqlDB *db, Duration d) {
    if (d < 0)
        d = 0;
    sync_mutex_lock(&db->mu);
    /* Wake the cleaner up when the lifetime is shortened. */
    if (d > 0 && d < sq_shortest_idle_time_locked(db))
        sq_poke_cleaner_locked(db);
    db->max_lifetime = d;
    sq_start_cleaner_locked(db);
    sync_mutex_unlock(&db->mu);
}

void sql_db_set_conn_max_idle_time(SqlDB *db, Duration d) {
    if (d < 0)
        d = 0;
    sync_mutex_lock(&db->mu);
    /* Wake the cleaner up when the idle time is shortened. */
    if (d > 0 && d < sq_shortest_idle_time_locked(db))
        sq_poke_cleaner_locked(db);
    db->max_idle_time = d;
    sq_start_cleaner_locked(db);
    sync_mutex_unlock(&db->mu);
}

SqlDBStats sql_db_stats(SqlDB *db) {
    Duration wait = (Duration)sync_atomic_int64_load(&db->wait_duration);
    sync_mutex_lock(&db->mu);
    SqlDBStats st = {
        .max_open_connections = db->max_open,
        .idle = db->free_conn.len,
        .open_connections = db->num_open,
        .in_use = db->num_open - db->free_conn.len,
        .wait_count = db->wait_count,
        .wait_duration = wait,
        .max_idle_closed = db->max_idle_closed,
        .max_idle_time_closed = db->max_idle_time_closed,
        .max_lifetime_closed = db->max_lifetime_closed,
    };
    sync_mutex_unlock(&db->mu);
    return st;
}

Driver sql_db_driver(SqlDB *db) {
    return db->connector.vt->driver(db->connector.data);
}

/* DB.Close. */
Error sql_db_close(SqlDB *db) {
    sync_mutex_lock(&db->mu);
    if (db->closed) { /* Make DB.Close idempotent */
        sync_mutex_unlock(&db->mu);
        return BURROW_NO_ERROR;
    }
    if (db->cleaner_ch != NULL)
        chan_close(db->cleaner_ch);
    SqDC *closing = NULL;
    SqDC **tail = &closing;
    for (Int i = 0; i < db->free_conn.len; i++) {
        SqDC *dc = (SqDC *)db->free_conn.p[i];
        sq_dc_close_db_locked(dc);
        dc->close_next = NULL;
        *tail = dc;
        tail = &dc->close_next;
        db->free_conn.p[i] = NULL;
    }
    db->free_conn.len = 0;
    db->closed = true;
    sq_reqs_close_all(db);
    db->opener_stop = true;
    sync_cond_broadcast(&db->opener_cond);
    sync_mutex_unlock(&db->mu);

    Error err = BURROW_NO_ERROR;
    while (closing != NULL) {
        SqDC *next = closing->close_next;
        closing->close_next = NULL;
        Error e = sq_dc_run_close_result(closing);
        if (BURROW_FAILED(e))
            err = e;
        closing = next;
    }
    BURROW_CALLF0(db->opener_cancel);
    if (db->connector.vt->close != NULL) {
        Error e = db->connector.vt->close(db->connector.data);
        if (BURROW_FAILED(e))
            err = e;
    }
    return err;
}

void sql_db_free(SqlDB *db) {
    if (db == NULL)
        return;
    (void)sql_db_close(db);
    sq_db_unref(db);
}

/* ------------------------------------------------------------ ctxutil.go */

static const DriverResult sq_no_result = {NULL, NULL};
static const DriverRows sq_no_rows = {NULL, NULL};

/* namedValueToValue. */
static Slice sq_named_to_values(Alloc *a, Slice named, Error *err) {
    Int n = named.len;
    DriverValue *v = NULL;
    if (n > 0) {
        v = BURROW_NEW_N(a, DriverValue, (size_t)n);
        if (v == NULL) {
            *err = burrow_err_out_of_memory;
            return slice_from(NULL, 0, 0, TYPE_ANY);
        }
    }
    const DriverNamedValue *p = (const DriverNamedValue *)named.p;
    for (Int i = 0; i < n; i++) {
        if (p[i].name.len > 0) {
            *err = sq_new_error(
                BURROW_S("sql: driver does not support the use of Named Parameters"));
            return slice_from(NULL, 0, 0, TYPE_ANY);
        }
        v[i] = p[i].value;
    }
    return slice_from(v, n, n, TYPE_ANY);
}

static DriverStmt sq_ctx_driver_prepare(Context ctx, DriverConn ci, Str query,
                                        Error *err) {
    Error e = BURROW_NO_ERROR;
    if (ci.vt->prepare_context != NULL) {
        DriverStmt si = ci.vt->prepare_context(ci.data, ctx, query, &e);
        *err = e;
        return si;
    }
    DriverStmt si = ci.vt->prepare(ci.data, query, &e);
    if (!BURROW_FAILED(e) && sq_ctx_done(ctx)) {
        si.vt->close(si.data);
        *err = context_err(ctx);
        return (DriverStmt){NULL, NULL};
    }
    *err = e;
    return si;
}

static DriverResult sq_ctx_driver_exec(Alloc *a, Alloc *tmp, Context ctx, DriverConn ci,
                                       Str query, Slice nvdargs, Error *err) {
    Error e = BURROW_NO_ERROR;
    if (ci.vt->exec_context != NULL) {
        DriverResult r = ci.vt->exec_context(ci.data, a, ctx, query, nvdargs, &e);
        *err = e;
        return r;
    }
    Slice dargs = sq_named_to_values(tmp, nvdargs, &e);
    if (BURROW_FAILED(e)) {
        *err = e;
        return sq_no_result;
    }
    if (sq_ctx_done(ctx)) {
        *err = context_err(ctx);
        return sq_no_result;
    }
    DriverResult r = ci.vt->exec(ci.data, a, query, dargs, &e);
    *err = e;
    return r;
}

static DriverRows sq_ctx_driver_query(Alloc *tmp, Context ctx, DriverConn ci, Str query,
                                      Slice nvdargs, Error *err) {
    Error e = BURROW_NO_ERROR;
    if (ci.vt->query_context != NULL) {
        DriverRows r = ci.vt->query_context(ci.data, ctx, query, nvdargs, &e);
        *err = e;
        return r;
    }
    Slice dargs = sq_named_to_values(tmp, nvdargs, &e);
    if (BURROW_FAILED(e)) {
        *err = e;
        return sq_no_rows;
    }
    if (sq_ctx_done(ctx)) {
        *err = context_err(ctx);
        return sq_no_rows;
    }
    DriverRows r = ci.vt->query(ci.data, query, dargs, &e);
    *err = e;
    return r;
}

static DriverResult sq_ctx_driver_stmt_exec(Alloc *a, Alloc *tmp, Context ctx,
                                            DriverStmt si, Slice nvdargs, Error *err) {
    Error e = BURROW_NO_ERROR;
    if (si.vt->exec_context != NULL) {
        DriverResult r = si.vt->exec_context(si.data, a, ctx, nvdargs, &e);
        *err = e;
        return r;
    }
    Slice dargs = sq_named_to_values(tmp, nvdargs, &e);
    if (BURROW_FAILED(e)) {
        *err = e;
        return sq_no_result;
    }
    if (sq_ctx_done(ctx)) {
        *err = context_err(ctx);
        return sq_no_result;
    }
    DriverResult r = si.vt->exec(si.data, a, dargs, &e);
    *err = e;
    return r;
}

static DriverRows sq_ctx_driver_stmt_query(Alloc *tmp, Context ctx, DriverStmt si,
                                           Slice nvdargs, Error *err) {
    Error e = BURROW_NO_ERROR;
    if (si.vt->query_context != NULL) {
        DriverRows r = si.vt->query_context(si.data, ctx, nvdargs, &e);
        *err = e;
        return r;
    }
    Slice dargs = sq_named_to_values(tmp, nvdargs, &e);
    if (BURROW_FAILED(e)) {
        *err = e;
        return sq_no_rows;
    }
    if (sq_ctx_done(ctx)) {
        *err = context_err(ctx);
        return sq_no_rows;
    }
    DriverRows r = si.vt->query(si.data, dargs, &e);
    *err = e;
    return r;
}

static DriverTx sq_ctx_driver_begin(Context ctx, const SqlTxOptions *opts,
                                    DriverConn ci, Error *err) {
    Error e = BURROW_NO_ERROR;
    if (ci.vt->begin_tx != NULL) {
        DriverTxOptions dopts = {0, false};
        if (opts != NULL) {
            dopts.isolation = (DriverIsolationLevel)opts->isolation;
            dopts.read_only = opts->read_only;
        }
        DriverTx txi = ci.vt->begin_tx(ci.data, ctx, dopts, &e);
        *err = e;
        return txi;
    }
    if (opts != NULL) {
        if (opts->isolation != SQL_LEVEL_DEFAULT) {
            *err = sq_new_error(
                BURROW_S("sql: driver does not support non-default isolation level"));
            return (DriverTx){NULL, NULL};
        }
        if (opts->read_only) {
            *err = sq_new_error(
                BURROW_S("sql: driver does not support read-only transactions"));
            return (DriverTx){NULL, NULL};
        }
    }
    DriverTx txi = ci.vt->begin(ci.data, &e);
    if (context_done(ctx) != NULL && !BURROW_FAILED(e) && sq_ctx_done(ctx)) {
        txi.vt->rollback(txi.data);
        *err = context_err(ctx);
        return (DriverTx){NULL, NULL};
    }
    *err = e;
    return txi;
}

/* -------------------------------------------------- releasing connections */

static void sq_release(SqRelease r, Error err) {
    if (r.kind == SQ_RELEASE_DC) {
        sq_put_conn((SqDC *)r.obj, err, true);
    } else if (r.kind == SQ_RELEASE_CONN) {
        /* Conn.closemuRUnlockCondReleaseConn. */
        SqlConn *c = (SqlConn *)r.obj;
        sq_closemu_runlock(&c->closemu);
        if (sq_bad_conn(err))
            (void)sq_conn_close(c, err);
        sq_conn_unref(c);
    } else if (r.kind == SQ_RELEASE_TX) {
        /* Tx.closemuRUnlockRelease. */
        SqlTx *tx = (SqlTx *)r.obj;
        sq_closemu_runlock(&tx->closemu);
        sq_tx_unref(tx);
    }
}

static SqRelease sq_release_dc(SqDC *dc) {
    return (SqRelease){SQ_RELEASE_DC, dc};
}

/* Conn.grabConn. Rechecks done under the lock, since a close can come in
 * between the first check and the lock and leave no connection behind. */
static SqDC *sq_conn_grab(SqlConn *c, SqRelease *rel, Error *err) {
    if (sync_atomic_bool_load(&c->done)) {
        *err = sql_err_conn_done;
        return NULL;
    }
    sq_closemu_rlock(&c->closemu);
    if (c->dc == NULL) {
        sq_closemu_runlock(&c->closemu);
        *err = sql_err_conn_done;
        return NULL;
    }
    sync_atomic_int64_add(&c->refs, 1);
    *rel = (SqRelease){SQ_RELEASE_CONN, c};
    *err = BURROW_NO_ERROR;
    return c->dc;
}

/* Tx.grabConn. */
static SqDC *sq_tx_grab(SqlTx *tx, Context ctx, SqRelease *rel, Error *err) {
    if (sq_ctx_done(ctx)) {
        *err = context_err(ctx);
        return NULL;
    }
    sq_closemu_rlock(&tx->closemu);
    if (sync_atomic_bool_load(&tx->done)) {
        sq_closemu_runlock(&tx->closemu);
        *err = sql_err_tx_done;
        return NULL;
    }
    if (burrow__sql_hook_tx_grab_conn != NULL) /* test hook */
        burrow__sql_hook_tx_grab_conn();
    sync_atomic_int64_add(&tx->refs, 1);
    *rel = (SqRelease){SQ_RELEASE_TX, tx};
    *err = BURROW_NO_ERROR;
    return tx->dc;
}

/* ------------------------------------------------------------- statements */

static void sq_stmt_ref(SqlStmt *s) {
    sync_atomic_int64_add(&s->refs, 1);
}

static SqlStmt *sq_stmt_new(SqlDB *db, Str query) {
    SqlStmt *s = BURROW_NEW(heap_allocator(), SqlStmt);
    if (s == NULL)
        return NULL;
    s->query = BURROW_STR_EMPTY;
    if (query.len > 0) {
        s->query = str_clone(heap_allocator(), query);
        if (s->query.p == NULL) {
            mem_free(heap_allocator(), s, sizeof(SqlStmt), _Alignof(SqlStmt));
            return NULL;
        }
    }
    arena_init(&s->sticky_arena, heap_allocator(), 0);
    sq_closemu_init(&s->closemu);
    sync_atomic_int64_store(&s->refs, 1);
    s->db = db;
    if (db != NULL)
        sq_db_ref(db);
    return s;
}

/* A Stmt that only remembers an error, Go's &Stmt{stickyErr: err}. */
static SqlStmt *sq_stmt_sticky(Error e) {
    SqlStmt *s = sq_stmt_new(NULL, BURROW_STR_EMPTY);
    if (s == NULL)
        return NULL;
    s->sticky = error_retain(arena_allocator(&s->sticky_arena), e);
    return s;
}

static void sq_stmt_unref(SqlStmt *s) {
    if (sync_atomic_int64_add(&s->refs, -1) != 0)
        return;
    if (s->query.len > 0)
        mem_free(heap_allocator(), (void *)(uintptr_t)s->query.p, (size_t)s->query.len,
                 1);
    arena_free(&s->sticky_arena);
    for (Int i = 0; i < s->css.len; i++)
        sq_ds_unref((SqDS *)s->css.p[i]);
    sq_vec_free(&s->css);
    if (s->cgds != NULL)
        sq_ds_unref(s->cgds);
    if (s->parent != NULL)
        sq_stmt_unref(s->parent);
    if (s->cg_kind == SQ_CG_CONN)
        sq_conn_unref((SqlConn *)s->cg);
    else if (s->cg_kind == SQ_CG_TX)
        sq_tx_unref((SqlTx *)s->cg);
    SqlDB *db = s->db;
    mem_free(heap_allocator(), s, sizeof(SqlStmt), _Alignof(SqlStmt));
    if (db != NULL)
        sq_db_unref(db);
}

static void sq_tx_add_stmt(SqlTx *tx, SqlStmt *s) {
    sq_stmt_ref(s);
    sync_mutex_lock(&tx->stmts_mu);
    s->tx_next = NULL;
    if (tx->stmts_last != NULL)
        tx->stmts_last->tx_next = s;
    else
        tx->stmts = s;
    tx->stmts_last = s;
    sync_mutex_unlock(&tx->stmts_mu);
}

/* noteUnusedDriverStatement. ds is closed now, or when its connection is
 * next returned if something is using it. */
static void sq_note_unused(SqDC *dc, SqDS *ds) {
    SqlDB *db = dc->db;
    sync_mutex_lock(&db->mu);
    if (dc->in_use) {
        sq_ds_ref(ds);
        ds->put_next = dc->on_put;
        dc->on_put = ds;
    } else {
        sync_mutex_lock(&dc->mu);
        bool fc = dc->final_closed;
        sync_mutex_unlock(&dc->mu);
        if (!fc)
            (void)sq_ds_close(ds);
    }
    sync_mutex_unlock(&db->mu);
}

/* Stmt.finalClose. */
static Error sq_stmt_final_close(SqlStmt *s) {
    sync_mutex_lock(&s->mu);
    for (Int i = 0; i < s->css.len; i++) {
        SqDS *ds = (SqDS *)s->css.p[i];
        sq_note_unused(ds->dc, ds);
        sq_dc_remove_open_stmt(ds->dc, ds);
        sq_ds_unref(ds);
    }
    sq_vec_free(&s->css);
    sync_mutex_unlock(&s->mu);
    return BURROW_NO_ERROR;
}

/* removeClosedStmtLocked. Drops the statements on connections that have
 * closed, but only once enough connections have closed to be worth a look. */
static void sq_stmt_remove_closed_locked(SqlStmt *s) {
    Int t = s->css.len / 2 + 1;
    if (t > 10)
        t = 10;
    int64_t db_closed = sync_atomic_int64_load(&s->db->num_closed);
    if (db_closed - s->last_num_closed < (int64_t)t)
        return;
    sync_mutex_lock(&s->db->mu);
    for (Int i = 0; i < s->css.len; i++) {
        SqDS *ds = (SqDS *)s->css.p[i];
        if (ds->dc->dbmu_closed) {
            s->css.p[i] = s->css.p[s->css.len - 1];
            s->css.p[s->css.len - 1] = NULL;
            s->css.len--;
            i--;
            sq_ds_unref(ds);
        }
    }
    sync_mutex_unlock(&s->db->mu);
    s->last_num_closed = db_closed;
}

/* prepareOnConnLocked. Returns a reference for the caller. */
static SqDS *sq_stmt_prepare_on_conn_locked(SqlStmt *s, Context ctx, SqDC *dc,
                                            Error *err) {
    SqDS *ds = sq_dc_prepare_locked(dc, ctx, s->cg_kind != SQ_CG_NONE, s->query, err);
    if (ds == NULL)
        return NULL;
    sync_mutex_lock(&s->mu);
    bool ok = sq_vec_push(&s->css, ds);
    sync_mutex_unlock(&s->mu);
    if (!ok) {
        ds->closed = true;
        ds->si.vt->close(ds->si.data);
        if (ds->in_open) {
            sq_dc_unlink_locked(dc, ds);
            sq_ds_unref(ds);
        }
        sq_ds_unref(ds);
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    sq_ds_ref(ds);
    return ds;
}

/* --------------------------------------------------------------- the *DC
 * functions, which run one thing on a connection and then release it. */

static DriverResult sq_result_from_statement(Alloc *a, Context ctx, DriverConn ci,
                                             SqDS *ds, Slice args, Error *err) {
    Error e = BURROW_NO_ERROR;
    DriverResult res = sq_no_result;
    Arena tmp;
    arena_init(&tmp, heap_allocator(), 0);
    sync_mutex_lock(&ds->dc->mu);
    Slice dargs = burrow__sql_driver_args_conn_locked(arena_allocator(&tmp), ci,
                                                      &ds->si, args, &e);
    if (!BURROW_FAILED(e))
        res = sq_ctx_driver_stmt_exec(a, arena_allocator(&tmp), ctx, ds->si, dargs, &e);
    sync_mutex_unlock(&ds->dc->mu);
    arena_free(&tmp);
    *err = e;
    return BURROW_FAILED(e) ? sq_no_result : res;
}

static DriverRows sq_rowsi_from_statement(Context ctx, DriverConn ci, SqDS *ds,
                                          Slice args, Error *err) {
    Error e = BURROW_NO_ERROR;
    DriverRows rowsi = sq_no_rows;
    Arena tmp;
    arena_init(&tmp, heap_allocator(), 0);
    sync_mutex_lock(&ds->dc->mu);
    Slice dargs = burrow__sql_driver_args_conn_locked(arena_allocator(&tmp), ci,
                                                      &ds->si, args, &e);
    if (!BURROW_FAILED(e))
        rowsi = sq_ctx_driver_stmt_query(arena_allocator(&tmp), ctx, ds->si, dargs, &e);
    sync_mutex_unlock(&ds->dc->mu);
    arena_free(&tmp);
    *err = e;
    return BURROW_FAILED(e) ? sq_no_rows : rowsi;
}

static Error sq_ping_dc(Context ctx, SqDC *dc, SqRelease rel) {
    Error err = BURROW_NO_ERROR;
    if (dc->ci.vt->ping != NULL) {
        sync_mutex_lock(&dc->mu);
        err = dc->ci.vt->ping(dc->ci.data, ctx);
        sync_mutex_unlock(&dc->mu);
    }
    sq_release(rel, err);
    return err;
}

static void sq_cg_ref(SqGrabber kind, void *cg) {
    if (kind == SQ_CG_CONN)
        sync_atomic_int64_add(&((SqlConn *)cg)->refs, 1);
    else if (kind == SQ_CG_TX)
        sync_atomic_int64_add(&((SqlTx *)cg)->refs, 1);
}

/* prepareDC. A Tx's statement goes on its list before the connection is let
 * go, so that a commit, which waits for the release, finds it there. */
static SqlStmt *sq_prepare_dc(SqlDB *db, Context ctx, SqDC *dc, SqRelease rel,
                              SqGrabber cg_kind, void *cg, Str query, SqlTx *add_to,
                              Error *err) {
    Error e = BURROW_NO_ERROR;
    sync_mutex_lock(&dc->mu);
    SqDS *ds = sq_dc_prepare_locked(dc, ctx, cg_kind != SQ_CG_NONE, query, &e);
    sync_mutex_unlock(&dc->mu);
    if (ds == NULL) {
        sq_release(rel, e);
        BURROW_OUT(err, e);
        return NULL;
    }
    SqlStmt *s = sq_stmt_new(db, query);
    if (s != NULL && cg_kind == SQ_CG_NONE && !sq_vec_push(&s->css, ds)) {
        sq_stmt_unref(s);
        s = NULL;
    }
    if (s == NULL) {
        sq_dc_remove_open_stmt(dc, ds);
        (void)sq_ds_close(ds);
        sq_ds_unref(ds);
        sq_release(rel, BURROW_NO_ERROR);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    s->cg_kind = cg_kind;
    s->cg = cg;
    sq_cg_ref(cg_kind, cg);
    if (cg_kind != SQ_CG_NONE) {
        s->cgds = ds;
    } else {
        s->last_num_closed = sync_atomic_int64_load(&db->num_closed);
        sq_stmt_add_dep(s);
    }
    if (add_to != NULL)
        sq_tx_add_stmt(add_to, s);
    sq_release(rel, BURROW_NO_ERROR);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return s;
}

/* execDC. */
static SqlResult sq_exec_dc(Alloc *a, Context ctx, SqDC *dc, SqRelease rel, Str query,
                            Slice args, Error *err) {
    Error e = BURROW_NO_ERROR;
    DriverConn ci = dc->ci;
    if (ci.vt->exec_context != NULL || ci.vt->exec != NULL) {
        DriverResult resi = sq_no_result;
        Arena tmp;
        arena_init(&tmp, heap_allocator(), 0);
        sync_mutex_lock(&dc->mu);
        Slice nvdargs = burrow__sql_driver_args_conn_locked(arena_allocator(&tmp), ci,
                                                            NULL, args, &e);
        if (!BURROW_FAILED(e))
            resi = sq_ctx_driver_exec(a, arena_allocator(&tmp), ctx, ci, query, nvdargs,
                                      &e);
        sync_mutex_unlock(&dc->mu);
        arena_free(&tmp);
        if (!sq_same(e, driver_err_skip)) {
            sq_release(rel, e);
            BURROW_OUT(err, e);
            return BURROW_FAILED(e) ? sq_no_result : resi;
        }
    }

    sync_mutex_lock(&dc->mu);
    DriverStmt si = sq_ctx_driver_prepare(ctx, ci, query, &e);
    sync_mutex_unlock(&dc->mu);
    if (BURROW_FAILED(e)) {
        sq_release(rel, e);
        BURROW_OUT(err, e);
        return sq_no_result;
    }
    SqDS *ds = sq_ds_new(dc, si);
    if (ds == NULL) {
        sync_mutex_lock(&dc->mu);
        si.vt->close(si.data);
        sync_mutex_unlock(&dc->mu);
        sq_release(rel, BURROW_NO_ERROR);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return sq_no_result;
    }
    SqlResult res = sq_result_from_statement(a, ctx, ci, ds, args, &e);
    (void)sq_ds_close(ds);
    sq_ds_unref(ds);
    sq_release(rel, e);
    BURROW_OUT(err, e);
    return res;
}

static SqlRows *sq_rows_new(SqDC *dc, SqRelease rel, DriverRows rowsi);
static bool sq_rows_init_context_close(SqlRows *rs, Context ctx, SqlTx *tx);
static Error sq_rows_close(SqlRows *rs, Error err);

/* Wraps rowsi, which came from dc, in a Rows, or closes everything and
 * releases dc when there is no memory for one. */
static SqlRows *sq_rows_wrap(Context ctx, SqlTx *txctx, SqDC *dc, SqRelease rel,
                             DriverRows rowsi, SqDS *close_stmt, SqlStmt *dep_stmt,
                             Error *err) {
    SqlRows *rs = sq_rows_new(dc, rel, rowsi);
    if (rs == NULL) {
        sync_mutex_lock(&dc->mu);
        rowsi.vt->close(rowsi.data);
        sync_mutex_unlock(&dc->mu);
        if (close_stmt != NULL) {
            (void)sq_ds_close(close_stmt);
            sq_ds_unref(close_stmt);
        }
        sq_release(rel, BURROW_NO_ERROR);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    rs->close_stmt = close_stmt;
    if (dep_stmt != NULL) {
        sq_stmt_add_dep(dep_stmt);
        sq_stmt_ref(dep_stmt);
        rs->dep_stmt = dep_stmt;
    }
    if (!sq_rows_init_context_close(rs, ctx, txctx)) {
        (void)sq_rows_close(rs, BURROW_NO_ERROR);
        sq_rows_unref(rs);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return rs;
}

/* queryDC. txctx is the Tx whose end also ends the Rows, or NULL. */
static SqlRows *sq_query_dc(Context ctx, SqlTx *txctx, SqDC *dc, SqRelease rel,
                            Str query, Slice args, Error *err) {
    Error e = BURROW_NO_ERROR;
    DriverConn ci = dc->ci;
    if (ci.vt->query_context != NULL || ci.vt->query != NULL) {
        DriverRows rowsi = sq_no_rows;
        Arena tmp;
        arena_init(&tmp, heap_allocator(), 0);
        sync_mutex_lock(&dc->mu);
        Slice nvdargs = burrow__sql_driver_args_conn_locked(arena_allocator(&tmp), ci,
                                                            NULL, args, &e);
        if (!BURROW_FAILED(e))
            rowsi =
                sq_ctx_driver_query(arena_allocator(&tmp), ctx, ci, query, nvdargs, &e);
        sync_mutex_unlock(&dc->mu);
        arena_free(&tmp);
        if (!sq_same(e, driver_err_skip)) {
            if (BURROW_FAILED(e)) {
                sq_release(rel, e);
                BURROW_OUT(err, e);
                return NULL;
            }
            return sq_rows_wrap(ctx, txctx, dc, rel, rowsi, NULL, NULL, err);
        }
    }

    sync_mutex_lock(&dc->mu);
    DriverStmt si = sq_ctx_driver_prepare(ctx, ci, query, &e);
    sync_mutex_unlock(&dc->mu);
    if (BURROW_FAILED(e)) {
        sq_release(rel, e);
        BURROW_OUT(err, e);
        return NULL;
    }
    SqDS *ds = sq_ds_new(dc, si);
    if (ds == NULL) {
        sync_mutex_lock(&dc->mu);
        si.vt->close(si.data);
        sync_mutex_unlock(&dc->mu);
        sq_release(rel, BURROW_NO_ERROR);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    DriverRows rowsi = sq_rowsi_from_statement(ctx, ci, ds, args, &e);
    if (BURROW_FAILED(e)) {
        (void)sq_ds_close(ds);
        sq_ds_unref(ds);
        sq_release(rel, e);
        BURROW_OUT(err, e);
        return NULL;
    }
    return sq_rows_wrap(ctx, txctx, dc, rel, rowsi, ds, NULL, err);
}

static Error sq_tx_rollback(SqlTx *tx, bool discard_conn);

/* Tx.awaitDone. Rolls the Tx back when its context ends, which a commit or a
 * rollback also does. */
static void sq_tx_await_done(void *env) {
    SqlTx *tx = (SqlTx *)env;
    (void)chan_recv(context_done(tx->ctx), NULL);
    ArenaMark m = error_mark();
    (void)sq_tx_rollback(tx, !tx->keep_conn_on_rollback);
    error_release(m);
    sq_tx_unref(tx);
}

/* beginDC. */
static SqlTx *sq_begin_dc(SqlDB *db, Context ctx, SqDC *dc, SqRelease rel,
                          const SqlTxOptions *opts, Error *err) {
    Error e = BURROW_NO_ERROR;
    sync_mutex_lock(&dc->mu);
    bool keep = dc->ci.vt->reset_session != NULL && dc->ci.vt->is_valid != NULL;
    DriverTx txi = sq_ctx_driver_begin(ctx, opts, dc->ci, &e);
    sync_mutex_unlock(&dc->mu);
    if (BURROW_FAILED(e)) {
        sq_release(rel, e);
        BURROW_OUT(err, e);
        return NULL;
    }

    SqlTx *tx = BURROW_NEW(heap_allocator(), SqlTx);
    ContextCancelFunc cancel = {NULL, NULL};
    Context tctx = {NULL, NULL};
    if (tx != NULL)
        tctx = context_with_cancel(heap_allocator(), ctx, &cancel);
    if (tx == NULL || BURROW_CONTEXT_IS_NIL(tctx)) {
        sync_mutex_lock(&dc->mu);
        Error re = txi.vt->rollback(txi.data);
        sync_mutex_unlock(&dc->mu);
        if (tx != NULL)
            mem_free(heap_allocator(), tx, sizeof(SqlTx), _Alignof(SqlTx));
        sq_release(rel, keep ? re : driver_err_bad_conn);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    tx->db = db;
    sq_db_ref(db);
    sq_closemu_init(&tx->closemu);
    tx->dc = dc;
    tx->txi = txi;
    tx->release = rel;
    tx->keep_conn_on_rollback = keep;
    tx->ctx = tctx;
    tx->cancel = cancel;
    sync_atomic_int64_store(&tx->refs, 2);
    if (!go(BURROW_FN(Func, sq_tx_await_done, tx))) {
        (void)sq_tx_rollback(tx, !keep);
        sq_tx_unref(tx);
        sq_tx_unref(tx);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return tx;
}

/* --------------------------------------------------------------------- DB */

#define SQ_RETRY(e, ...)                                                               \
    for (int sq_i = 0;; sq_i++) {                                                      \
        int strategy = sq_i < 2 ? SQ_CACHED_OR_NEW : SQ_ALWAYS_NEW;                    \
        __VA_ARGS__;                                                                   \
        if (sq_i == 2 || !sq_bad_conn(e))                                              \
            break;                                                                     \
    }

SqlDB *sql_open_db(DriverConnector c) {
    SqlDB *db = BURROW_NEW(heap_allocator(), SqlDB);
    if (db == NULL)
        return NULL;
    db->connector = c;
    db->opener_cond = SYNC_COND(sync_mutex_locker(&db->mu));
    ContextCancelFunc cancel = {NULL, NULL};
    db->opener_ctx =
        context_with_cancel(heap_allocator(), context_background(), &cancel);
    if (BURROW_CONTEXT_IS_NIL(db->opener_ctx)) {
        mem_free(heap_allocator(), db, sizeof(SqlDB), _Alignof(SqlDB));
        return NULL;
    }
    db->opener_cancel = cancel;
    sync_atomic_int64_store(&db->refs, 2);
    if (!go(BURROW_FN(Func, sq_opener, db))) {
        BURROW_CALLF0(cancel);
        context_release(db->opener_ctx);
        mem_free(heap_allocator(), db, sizeof(SqlDB), _Alignof(SqlDB));
        return NULL;
    }
    return db;
}

SqlDB *sql_open(Str driver_name, Str data_source_name, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    bool ok = false;
    Driver d = burrow__sql_driver_lookup(driver_name, &ok);
    if (!ok) {
        BURROW_OUT(err, fmt_errorf_v("sql: unknown driver %q (forgotten import?)",
                                     driver_name));
        return NULL;
    }
    if (d.vt->open_connector != NULL) {
        Error e = BURROW_NO_ERROR;
        DriverConnector c = d.vt->open_connector(d.data, data_source_name, &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return NULL;
        }
        SqlDB *db = sql_open_db(c);
        if (db == NULL && c.vt->close != NULL)
            (void)c.vt->close(c.data);
        return db;
    }
    SqDsn *t = BURROW_NEW(heap_allocator(), SqDsn);
    if (t == NULL)
        return NULL;
    t->dsn = BURROW_STR_EMPTY;
    if (data_source_name.len > 0) {
        t->dsn = str_clone(heap_allocator(), data_source_name);
        if (t->dsn.p == NULL) {
            mem_free(heap_allocator(), t, sizeof(SqDsn), _Alignof(SqDsn));
            return NULL;
        }
    }
    t->driver = d;
    SqlDB *db = sql_open_db((DriverConnector){&sq_dsn_vt, t});
    if (db == NULL) {
        if (t->dsn.len > 0)
            mem_free(heap_allocator(), (void *)(uintptr_t)t->dsn.p, (size_t)t->dsn.len,
                     1);
        mem_free(heap_allocator(), t, sizeof(SqDsn), _Alignof(SqDsn));
        return NULL;
    }
    db->dsn = t;
    return db;
}

Error sql_db_ping_context(SqlDB *db, Context ctx) {
    Error e = BURROW_NO_ERROR;
    SqDC *dc = NULL;
    SQ_RETRY(e, dc = sq_conn(db, ctx, strategy, &e));
    if (BURROW_FAILED(e))
        return e;
    return sq_ping_dc(ctx, dc, sq_release_dc(dc));
}

Error sql_db_ping(SqlDB *db) {
    return sql_db_ping_context(db, context_background());
}

SqlStmt *sql_db_prepare_context(SqlDB *db, Context ctx, Str query, Error *err) {
    Error e = BURROW_NO_ERROR;
    SqlStmt *s = NULL;
    SQ_RETRY(e, {
        SqDC *dc = sq_conn(db, ctx, strategy, &e);
        if (dc != NULL)
            s = sq_prepare_dc(db, ctx, dc, sq_release_dc(dc), SQ_CG_NONE, NULL, query,
                              NULL, &e);
    });
    BURROW_OUT(err, e);
    return s;
}

SqlStmt *sql_db_prepare(SqlDB *db, Str query, Error *err) {
    return sql_db_prepare_context(db, context_background(), query, err);
}

SqlResult sql_db_exec_context(SqlDB *db, Alloc *a, Context ctx, Str query, Slice args,
                              Error *err) {
    Error e = BURROW_NO_ERROR;
    SqlResult res = sq_no_result;
    SQ_RETRY(e, {
        SqDC *dc = sq_conn(db, ctx, strategy, &e);
        if (dc != NULL)
            res = sq_exec_dc(a, ctx, dc, sq_release_dc(dc), query, args, &e);
    });
    BURROW_OUT(err, e);
    return res;
}

SqlResult sql_db_exec(SqlDB *db, Alloc *a, Str query, Slice args, Error *err) {
    return sql_db_exec_context(db, a, context_background(), query, args, err);
}

SqlRows *sql_db_query_context(SqlDB *db, Context ctx, Str query, Slice args,
                              Error *err) {
    Error e = BURROW_NO_ERROR;
    SqlRows *rs = NULL;
    SQ_RETRY(e, {
        SqDC *dc = sq_conn(db, ctx, strategy, &e);
        if (dc != NULL)
            rs = sq_query_dc(ctx, NULL, dc, sq_release_dc(dc), query, args, &e);
    });
    BURROW_OUT(err, e);
    return rs;
}

SqlRows *sql_db_query(SqlDB *db, Str query, Slice args, Error *err) {
    return sql_db_query_context(db, context_background(), query, args, err);
}

SqlRow sql_db_query_row_context(SqlDB *db, Context ctx, Str query, Slice args) {
    Error e = BURROW_NO_ERROR;
    SqlRows *rs = sql_db_query_context(db, ctx, query, args, &e);
    return (SqlRow){e, rs};
}

SqlRow sql_db_query_row(SqlDB *db, Str query, Slice args) {
    return sql_db_query_row_context(db, context_background(), query, args);
}

SqlTx *sql_db_begin_tx(SqlDB *db, Context ctx, const SqlTxOptions *opts, Error *err) {
    Error e = BURROW_NO_ERROR;
    SqlTx *tx = NULL;
    SQ_RETRY(e, {
        SqDC *dc = sq_conn(db, ctx, strategy, &e);
        if (dc != NULL)
            tx = sq_begin_dc(db, ctx, dc, sq_release_dc(dc), opts, &e);
    });
    BURROW_OUT(err, e);
    return tx;
}

SqlTx *sql_db_begin(SqlDB *db, Error *err) {
    return sql_db_begin_tx(db, context_background(), NULL, err);
}

SqlConn *sql_db_conn(SqlDB *db, Context ctx, Error *err) {
    Error e = BURROW_NO_ERROR;
    SqDC *dc = NULL;
    SQ_RETRY(e, dc = sq_conn(db, ctx, strategy, &e));
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    SqlConn *c = BURROW_NEW(heap_allocator(), SqlConn);
    if (c == NULL) {
        sq_put_conn(dc, BURROW_NO_ERROR, true);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    c->db = db;
    sq_db_ref(db);
    c->dc = dc;
    sq_closemu_init(&c->closemu);
    sync_atomic_int64_store(&c->refs, 1);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return c;
}

/* ------------------------------------------------------------------- Conn */

static void sq_conn_unref(SqlConn *c) {
    if (sync_atomic_int64_add(&c->refs, -1) != 0)
        return;
    SqlDB *db = c->db;
    mem_free(heap_allocator(), c, sizeof(SqlConn), _Alignof(SqlConn));
    sq_db_unref(db);
}

/* Conn.close. */
static Error sq_conn_close(SqlConn *c, Error err) {
    if (!sync_atomic_bool_compare_and_swap(&c->done, false, true))
        return sql_err_conn_done;
    sq_closemu_lock(&c->closemu);
    SqDC *dc = c->dc;
    c->dc = NULL;
    sq_put_conn(dc, err, true);
    sq_closemu_unlock(&c->closemu);
    return err;
}

Error sql_conn_close(SqlConn *c) {
    return sq_conn_close(c, BURROW_NO_ERROR);
}

void sql_conn_free(SqlConn *c) {
    if (c == NULL)
        return;
    (void)sql_conn_close(c);
    sq_conn_unref(c);
}

Error sql_conn_ping_context(SqlConn *c, Context ctx) {
    SqRelease rel = {SQ_RELEASE_NONE, NULL};
    Error e = BURROW_NO_ERROR;
    SqDC *dc = sq_conn_grab(c, &rel, &e);
    if (dc == NULL)
        return e;
    return sq_ping_dc(ctx, dc, rel);
}

SqlResult sql_conn_exec_context(SqlConn *c, Alloc *a, Context ctx, Str query,
                                Slice args, Error *err) {
    SqRelease rel = {SQ_RELEASE_NONE, NULL};
    Error e = BURROW_NO_ERROR;
    SqDC *dc = sq_conn_grab(c, &rel, &e);
    if (dc == NULL) {
        BURROW_OUT(err, e);
        return sq_no_result;
    }
    return sq_exec_dc(a, ctx, dc, rel, query, args, err);
}

SqlRows *sql_conn_query_context(SqlConn *c, Context ctx, Str query, Slice args,
                                Error *err) {
    SqRelease rel = {SQ_RELEASE_NONE, NULL};
    Error e = BURROW_NO_ERROR;
    SqDC *dc = sq_conn_grab(c, &rel, &e);
    if (dc == NULL) {
        BURROW_OUT(err, e);
        return NULL;
    }
    return sq_query_dc(ctx, NULL, dc, rel, query, args, err);
}

SqlRow sql_conn_query_row_context(SqlConn *c, Context ctx, Str query, Slice args) {
    Error e = BURROW_NO_ERROR;
    SqlRows *rs = sql_conn_query_context(c, ctx, query, args, &e);
    return (SqlRow){e, rs};
}

SqlStmt *sql_conn_prepare_context(SqlConn *c, Context ctx, Str query, Error *err) {
    SqRelease rel = {SQ_RELEASE_NONE, NULL};
    Error e = BURROW_NO_ERROR;
    SqDC *dc = sq_conn_grab(c, &rel, &e);
    if (dc == NULL) {
        BURROW_OUT(err, e);
        return NULL;
    }
    return sq_prepare_dc(c->db, ctx, dc, rel, SQ_CG_CONN, c, query, NULL, err);
}

SqlTx *sql_conn_begin_tx(SqlConn *c, Context ctx, const SqlTxOptions *opts,
                         Error *err) {
    SqRelease rel = {SQ_RELEASE_NONE, NULL};
    Error e = BURROW_NO_ERROR;
    SqDC *dc = sq_conn_grab(c, &rel, &e);
    if (dc == NULL) {
        BURROW_OUT(err, e);
        return NULL;
    }
    return sq_begin_dc(c->db, ctx, dc, rel, opts, err);
}

static void sq_raw_call(SqlRawFunc f, DriverConn ci, Error *out) {
    *out = BURROW_CALLF(f, ci);
}

/* Conn.Raw. A panic in f leaves the connection marked bad and carries on up
 * once the connection is released. */
Error sql_conn_raw(SqlConn *c, SqlRawFunc f) {
    SqRelease rel = {SQ_RELEASE_NONE, NULL};
    Error e = BURROW_NO_ERROR;
    SqDC *dc = sq_conn_grab(c, &rel, &e);
    if (dc == NULL)
        return e;
    Error res[1] = {BURROW_NO_ERROR};
    Any saved[1] = {{NULL, NULL}};
    volatile bool panicked = false;
    sync_mutex_lock(&dc->mu);
    BURROW_TRY {
        sq_raw_call(f, dc->ci, res);
    }
    BURROW_CATCH(p) {
        saved[0] = p;
        panicked = true;
    }
    BURROW_TRY_END;
    sync_mutex_unlock(&dc->mu);
    if (panicked) {
        sq_release(rel, driver_err_bad_conn);
        panic(saved[0]);
    }
    sq_release(rel, res[0]);
    return res[0];
}

/* --------------------------------------------------------------------- Tx */

static void sq_tx_unref(SqlTx *tx) {
    if (sync_atomic_int64_add(&tx->refs, -1) != 0)
        return;
    context_release(tx->ctx);
    SqlDB *db = tx->db;
    mem_free(heap_allocator(), tx, sizeof(SqlTx), _Alignof(SqlTx));
    sq_db_unref(db);
}

/* closePrepared, and letting go of the list either way. */
static void sq_tx_end_stmts(SqlTx *tx, bool close) {
    sync_mutex_lock(&tx->stmts_mu);
    SqlStmt *l = tx->stmts;
    tx->stmts = NULL;
    tx->stmts_last = NULL;
    sync_mutex_unlock(&tx->stmts_mu);
    while (l != NULL) {
        SqlStmt *next = l->tx_next;
        l->tx_next = NULL;
        if (close)
            (void)sql_stmt_close(l);
        sq_stmt_unref(l);
        l = next;
    }
}

/* Tx.close. */
static void sq_tx_close(SqlTx *tx, Error err) {
    SqRelease rel = tx->release;
    tx->release = (SqRelease){SQ_RELEASE_NONE, NULL};
    sq_release(rel, err);
    tx->dc = NULL;
}

Error sql_tx_commit(SqlTx *tx) {
    /* Check context first to avoid transaction leak. */
    if (sq_ctx_done(tx->ctx)) {
        if (sync_atomic_bool_load(&tx->done))
            return sql_err_tx_done;
        return context_err(tx->ctx);
    }
    if (!sync_atomic_bool_compare_and_swap(&tx->done, false, true))
        return sql_err_tx_done;
    /* Cancel the Tx to release any active R-closemu locks. */
    BURROW_CALLF0(tx->cancel);
    sq_closemu_lock(&tx->closemu);
    sq_closemu_unlock(&tx->closemu);

    sync_mutex_lock(&tx->dc->mu);
    Error err = tx->txi.vt->commit(tx->txi.data);
    sync_mutex_unlock(&tx->dc->mu);
    sq_tx_end_stmts(tx, !sq_bad_conn(err));
    sq_tx_close(tx, err);
    return err;
}

/* Tx.rollback. */
static Error sq_tx_rollback(SqlTx *tx, bool discard_conn) {
    if (!sync_atomic_bool_compare_and_swap(&tx->done, false, true))
        return sql_err_tx_done;
    if (burrow__sql_rollback_hook != NULL)
        burrow__sql_rollback_hook();
    BURROW_CALLF0(tx->cancel);
    sq_closemu_lock(&tx->closemu);
    sq_closemu_unlock(&tx->closemu);

    sync_mutex_lock(&tx->dc->mu);
    Error err = tx->txi.vt->rollback(tx->txi.data);
    sync_mutex_unlock(&tx->dc->mu);
    sq_tx_end_stmts(tx, !sq_bad_conn(err));
    if (discard_conn)
        err = driver_err_bad_conn;
    sq_tx_close(tx, err);
    return err;
}

Error sql_tx_rollback(SqlTx *tx) {
    return sq_tx_rollback(tx, false);
}

void sql_tx_free(SqlTx *tx) {
    if (tx == NULL)
        return;
    if (!sync_atomic_bool_load(&tx->done))
        (void)sq_tx_rollback(tx, false);
    sq_tx_unref(tx);
}

SqlStmt *sql_tx_prepare_context(SqlTx *tx, Context ctx, Str query, Error *err) {
    SqRelease rel = {SQ_RELEASE_NONE, NULL};
    Error e = BURROW_NO_ERROR;
    SqDC *dc = sq_tx_grab(tx, ctx, &rel, &e);
    if (dc == NULL) {
        BURROW_OUT(err, e);
        return NULL;
    }
    return sq_prepare_dc(tx->db, ctx, dc, rel, SQ_CG_TX, tx, query, tx, err);
}

SqlStmt *sql_tx_prepare(SqlTx *tx, Str query, Error *err) {
    return sql_tx_prepare_context(tx, context_background(), query, err);
}

SqlResult sql_tx_exec_context(SqlTx *tx, Alloc *a, Context ctx, Str query, Slice args,
                              Error *err) {
    SqRelease rel = {SQ_RELEASE_NONE, NULL};
    Error e = BURROW_NO_ERROR;
    SqDC *dc = sq_tx_grab(tx, ctx, &rel, &e);
    if (dc == NULL) {
        BURROW_OUT(err, e);
        return sq_no_result;
    }
    return sq_exec_dc(a, ctx, dc, rel, query, args, err);
}

SqlResult sql_tx_exec(SqlTx *tx, Alloc *a, Str query, Slice args, Error *err) {
    return sql_tx_exec_context(tx, a, context_background(), query, args, err);
}

SqlRows *sql_tx_query_context(SqlTx *tx, Context ctx, Str query, Slice args,
                              Error *err) {
    SqRelease rel = {SQ_RELEASE_NONE, NULL};
    Error e = BURROW_NO_ERROR;
    SqDC *dc = sq_tx_grab(tx, ctx, &rel, &e);
    if (dc == NULL) {
        BURROW_OUT(err, e);
        return NULL;
    }
    return sq_query_dc(ctx, tx, dc, rel, query, args, err);
}

SqlRows *sql_tx_query(SqlTx *tx, Str query, Slice args, Error *err) {
    return sql_tx_query_context(tx, context_background(), query, args, err);
}

SqlRow sql_tx_query_row_context(SqlTx *tx, Context ctx, Str query, Slice args) {
    Error e = BURROW_NO_ERROR;
    SqlRows *rs = sql_tx_query_context(tx, ctx, query, args, &e);
    return (SqlRow){e, rs};
}

SqlRow sql_tx_query_row(SqlTx *tx, Str query, Slice args) {
    return sql_tx_query_row_context(tx, context_background(), query, args);
}

/* Tx.StmtContext. A statement from the DB shares the driver statement it
 * already has on the Tx's connection, preparing one there if it has none,
 * and keeps the DB statement open until it is closed itself. */
SqlStmt *sql_tx_stmt_context(SqlTx *tx, Context ctx, SqlStmt *stmt) {
    SqRelease rel = {SQ_RELEASE_NONE, NULL};
    Error e = BURROW_NO_ERROR;
    SqDC *dc = sq_tx_grab(tx, ctx, &rel, &e);
    if (dc == NULL)
        return sq_stmt_sticky(e);
    if (tx->db != stmt->db) {
        sq_release(rel, BURROW_NO_ERROR);
        return sq_stmt_sticky(sq_new_error(
            BURROW_S("sql: Tx.Stmt: statement from different database used")));
    }
    SqDS *ds = NULL;
    SqlStmt *parent = NULL;
    sync_mutex_lock(&stmt->mu);
    if (stmt->closed || stmt->cg_kind != SQ_CG_NONE) {
        sync_mutex_unlock(&stmt->mu);
        sync_mutex_lock(&dc->mu);
        DriverStmt si = sq_ctx_driver_prepare(ctx, dc->ci, stmt->query, &e);
        sync_mutex_unlock(&dc->mu);
        if (BURROW_FAILED(e)) {
            SqlStmt *res = sq_stmt_sticky(e);
            sq_release(rel, BURROW_NO_ERROR);
            return res;
        }
        ds = sq_ds_new(dc, si);
        if (ds == NULL) {
            sync_mutex_lock(&dc->mu);
            si.vt->close(si.data);
            sync_mutex_unlock(&dc->mu);
            sq_release(rel, BURROW_NO_ERROR);
            return NULL;
        }
    } else {
        sq_stmt_remove_closed_locked(stmt);
        for (Int i = 0; i < stmt->css.len; i++) {
            SqDS *v = (SqDS *)stmt->css.p[i];
            if (v->dc == dc) {
                ds = v;
                sq_ds_ref(ds);
                break;
            }
        }
        sync_mutex_unlock(&stmt->mu);
        if (ds == NULL) {
            sync_mutex_lock(&dc->mu);
            ds = sq_stmt_prepare_on_conn_locked(stmt, ctx, dc, &e);
            sync_mutex_unlock(&dc->mu);
            if (ds == NULL) {
                SqlStmt *res = sq_stmt_sticky(e);
                sq_release(rel, BURROW_NO_ERROR);
                return res;
            }
        }
        parent = stmt;
    }
    SqlStmt *txs = sq_stmt_new(tx->db, stmt->query);
    if (txs == NULL) {
        if (parent == NULL)
            (void)sq_ds_close(ds);
        sq_ds_unref(ds);
        sq_release(rel, BURROW_NO_ERROR);
        return NULL;
    }
    txs->cg_kind = SQ_CG_TX;
    txs->cg = tx;
    sync_atomic_int64_add(&tx->refs, 1);
    txs->cgds = ds;
    if (parent != NULL) {
        txs->parent = parent;
        sq_stmt_ref(parent);
        sq_stmt_add_dep(parent);
    }
    sq_tx_add_stmt(tx, txs);
    sq_release(rel, BURROW_NO_ERROR);
    return txs;
}

SqlStmt *sql_tx_stmt(SqlTx *tx, SqlStmt *stmt) {
    return sql_tx_stmt_context(tx, context_background(), stmt);
}

/* ------------------------------------------------------------------- Stmt */

/* connStmt. The connection to run s on and the driver statement for it, which
 * comes with a reference. */
static SqDC *sq_stmt_conn_stmt(SqlStmt *s, Context ctx, int strategy, SqRelease *rel,
                               SqDS **dsp, Error *err) {
    if (BURROW_FAILED(s->sticky)) {
        *err = s->sticky;
        return NULL;
    }
    sync_mutex_lock(&s->mu);
    if (s->closed) {
        sync_mutex_unlock(&s->mu);
        *err = sq_new_error(BURROW_S("sql: statement is closed"));
        return NULL;
    }
    if (s->cg_kind != SQ_CG_NONE) {
        sync_mutex_unlock(&s->mu);
        SqDC *dc = s->cg_kind == SQ_CG_CONN ? sq_conn_grab((SqlConn *)s->cg, rel, err)
                                            : sq_tx_grab((SqlTx *)s->cg, ctx, rel, err);
        if (dc == NULL)
            return NULL;
        sq_ds_ref(s->cgds);
        *dsp = s->cgds;
        return dc;
    }
    sq_stmt_remove_closed_locked(s);
    sync_mutex_unlock(&s->mu);

    SqDC *dc = sq_conn(s->db, ctx, strategy, err);
    if (dc == NULL)
        return NULL;
    sync_mutex_lock(&s->mu);
    for (Int i = 0; i < s->css.len; i++) {
        SqDS *v = (SqDS *)s->css.p[i];
        if (v->dc == dc) {
            sq_ds_ref(v);
            sync_mutex_unlock(&s->mu);
            *rel = sq_release_dc(dc);
            *dsp = v;
            return dc;
        }
    }
    sync_mutex_unlock(&s->mu);

    Error e = BURROW_NO_ERROR;
    sync_mutex_lock(&dc->mu);
    SqDS *ds = sq_stmt_prepare_on_conn_locked(s, ctx, dc, &e);
    sync_mutex_unlock(&dc->mu);
    if (ds == NULL) {
        sq_put_conn(dc, e, true);
        *err = e;
        return NULL;
    }
    *rel = sq_release_dc(dc);
    *dsp = ds;
    *err = BURROW_NO_ERROR;
    return dc;
}

SqlResult sql_stmt_exec_context(SqlStmt *s, Alloc *a, Context ctx, Slice args,
                                Error *err) {
    sq_closemu_rlock(&s->closemu);
    Error e = BURROW_NO_ERROR;
    SqlResult res = sq_no_result;
    SQ_RETRY(e, {
        SqRelease rel = {SQ_RELEASE_NONE, NULL};
        SqDS *ds = NULL;
        SqDC *dc = sq_stmt_conn_stmt(s, ctx, strategy, &rel, &ds, &e);
        if (dc != NULL) {
            res = sq_result_from_statement(a, ctx, dc->ci, ds, args, &e);
            sq_ds_unref(ds);
            sq_release(rel, e);
        }
    });
    sq_closemu_runlock(&s->closemu);
    BURROW_OUT(err, e);
    return res;
}

SqlResult sql_stmt_exec(SqlStmt *s, Alloc *a, Slice args, Error *err) {
    return sql_stmt_exec_context(s, a, context_background(), args, err);
}

SqlRows *sql_stmt_query_context(SqlStmt *s, Context ctx, Slice args, Error *err) {
    sq_closemu_rlock(&s->closemu);
    Error e = BURROW_NO_ERROR;
    SqlRows *rows = NULL;
    SQ_RETRY(e, {
        SqRelease rel = {SQ_RELEASE_NONE, NULL};
        SqDS *ds = NULL;
        SqDC *dc = sq_stmt_conn_stmt(s, ctx, strategy, &rel, &ds, &e);
        if (dc != NULL) {
            DriverRows rowsi = sq_rowsi_from_statement(ctx, dc->ci, ds, args, &e);
            sq_ds_unref(ds);
            if (BURROW_FAILED(e)) {
                sq_release(rel, e);
            } else {
                SqlTx *txctx = s->cg_kind == SQ_CG_TX ? (SqlTx *)s->cg : NULL;
                rows = sq_rows_wrap(ctx, txctx, dc, rel, rowsi, NULL, s, &e);
            }
        }
    });
    sq_closemu_runlock(&s->closemu);
    BURROW_OUT(err, e);
    return rows;
}

SqlRows *sql_stmt_query(SqlStmt *s, Slice args, Error *err) {
    return sql_stmt_query_context(s, context_background(), args, err);
}

SqlRow sql_stmt_query_row_context(SqlStmt *s, Context ctx, Slice args) {
    Error e = BURROW_NO_ERROR;
    SqlRows *rs = sql_stmt_query_context(s, ctx, args, &e);
    return (SqlRow){e, rs};
}

SqlRow sql_stmt_query_row(SqlStmt *s, Slice args) {
    return sql_stmt_query_row_context(s, context_background(), args);
}

Error sql_stmt_close(SqlStmt *s) {
    sq_closemu_lock(&s->closemu);
    if (BURROW_FAILED(s->sticky)) {
        sq_closemu_unlock(&s->closemu);
        return s->sticky;
    }
    sync_mutex_lock(&s->mu);
    if (s->closed) {
        sync_mutex_unlock(&s->mu);
        sq_closemu_unlock(&s->closemu);
        return BURROW_NO_ERROR;
    }
    s->closed = true;
    SqDS *txds = s->cgds;
    s->cgds = NULL;
    sync_mutex_unlock(&s->mu);

    Error err = BURROW_NO_ERROR;
    if (s->cg_kind == SQ_CG_NONE) {
        err = sq_stmt_remove_dep(s);
    } else if (s->parent != NULL) {
        /* txds is in the parent's css, which closes it. */
        err = sq_stmt_remove_dep(s->parent);
    } else {
        err = sq_ds_close(txds);
    }
    if (txds != NULL)
        sq_ds_unref(txds);
    sq_closemu_unlock(&s->closemu);
    return err;
}

void sql_stmt_free(SqlStmt *s) {
    if (s == NULL)
        return;
    (void)sql_stmt_close(s);
    sq_stmt_unref(s);
}

/* ------------------------------------------------------------------- Rows */

static SqlRows *sq_rows_new(SqDC *dc, SqRelease rel, DriverRows rowsi) {
    SqlRows *rs = BURROW_NEW(heap_allocator(), SqlRows);
    if (rs == NULL)
        return NULL;
    rs->dc = dc;
    sync_atomic_int64_add(&dc->refs, 1);
    rs->release = rel;
    rs->rowsi = rowsi;
    sq_closemu_init(&rs->closemu);
    rs->raw = slice_from(NULL, 0, 0, TYPE_BYTE);
    arena_init(&rs->raw_arena, heap_allocator(), 0);
    arena_init(&rs->err_arena, heap_allocator(), 0);
    sync_atomic_int64_store(&rs->refs, 1);
    return rs;
}

static void sq_rows_free_lastcols(SqlRows *rs) {
    if (rs->lastcols != NULL)
        mem_free(heap_allocator(), rs->lastcols,
                 (size_t)rs->lastcols_len * sizeof(DriverValue), _Alignof(DriverValue));
    rs->lastcols = NULL;
    rs->lastcols_len = 0;
}

static void sq_rows_unref(SqlRows *rs) {
    if (sync_atomic_int64_add(&rs->refs, -1) != 0)
        return;
    SqlRows *c = rs->children;
    while (c != NULL) {
        SqlRows *next = c->child_next;
        sq_rows_unref(c);
        c = next;
    }
    if (rs->has_ctx)
        context_release(rs->closectx);
    if (rs->await_tx != NULL)
        sq_tx_unref(rs->await_tx);
    if (rs->close_stmt != NULL)
        sq_ds_unref(rs->close_stmt);
    if (rs->dep_stmt != NULL)
        sq_stmt_unref(rs->dep_stmt);
    sq_rows_free_lastcols(rs);
    arena_free(&rs->raw_arena);
    arena_free(&rs->err_arena);
    SqDC *dc = rs->dc;
    mem_free(heap_allocator(), rs, sizeof(SqlRows), _Alignof(SqlRows));
    sq_dc_unref(dc);
}

/* An error the Rows keeps past the call that made it. */
static Error sq_rows_keep(SqlRows *rs, Error e) {
    if (!BURROW_FAILED(e))
        return e;
    return error_retain(arena_allocator(&rs->err_arena), e);
}

/* lasterrOrErrLocked. */
static Error sq_rows_lasterr_or(const SqlRows *rs, Error err) {
    if (BURROW_FAILED(rs->lasterr) && !sq_same(rs->lasterr, io_eof))
        return rs->lasterr;
    return err;
}

/* Rows.awaitDone. Go selects on the query's context as well, which here may
 * be gone by the time it is done, so closectx stands in for it, and closing
 * tells a cancel from Close apart from the query's context ending. */
static void sq_rows_await_done(void *env) {
    SqlRows *rs = (SqlRows *)env;
    Chan *txd = rs->await_tx != NULL ? context_done(rs->await_tx->ctx) : NULL;
    SelectCase cases[2] = {BURROW_RECV(context_done(rs->closectx), NULL),
                           BURROW_RECV(txd, NULL)};
    Int which = chan_select(cases, 2);
    ArenaMark m = error_mark();
    if (which == 0 && !sync_atomic_bool_load(&rs->closing)) {
        rs->context_done = context_err(rs->closectx);
        sync_atomic_bool_store(&rs->has_cd, true);
    } else if (which == 1) {
        rs->context_done = context_err(rs->await_tx->ctx);
        sync_atomic_bool_store(&rs->has_cd, true);
    }
    (void)sq_rows_close(rs, context_err(rs->closectx));
    error_release(m);
    sq_rows_unref(rs);
}

/* initContextClose. False when there was no memory for it. */
static bool sq_rows_init_context_close(SqlRows *rs, Context ctx, SqlTx *tx) {
    Chan *txd = tx != NULL ? context_done(tx->ctx) : NULL;
    if (context_done(ctx) == NULL && txd == NULL)
        return true;
    if (burrow__sql_bypass_rows_await_done)
        return true;
    ContextCancelFunc cancel = {NULL, NULL};
    Context closectx = context_with_cancel(heap_allocator(), ctx, &cancel);
    if (BURROW_CONTEXT_IS_NIL(closectx))
        return false;
    rs->closectx = closectx;
    rs->cancel = cancel;
    rs->has_ctx = true;
    if (tx != NULL) {
        sync_atomic_int64_add(&tx->refs, 1);
        rs->await_tx = tx;
    }
    sync_atomic_int64_add(&rs->refs, 1);
    if (!go(BURROW_FN(Func, sq_rows_await_done, rs))) {
        sync_atomic_int64_add(&rs->refs, -1);
        return false;
    }
    return true;
}

/* Rows.close. The cursors that came out of rs are closed with it, newest
 * first, which is the order Go's chain of cancel funcs closes them in. */
static Error sq_rows_close(SqlRows *rs, Error err) {
    sq_closemu_lock(&rs->closemu);
    if (rs->closed) {
        sq_closemu_unlock(&rs->closemu);
        return BURROW_NO_ERROR;
    }
    rs->closed = true;
    if (!BURROW_FAILED(rs->lasterr))
        rs->lasterr = sq_rows_keep(rs, err);

    sync_mutex_lock(&rs->dc->mu);
    Error e = rs->rowsi.vt->close(rs->rowsi.data);
    sync_mutex_unlock(&rs->dc->mu);
    burrow__SqlRowsCloseHook hook = burrow__sql_rows_close_hook;
    if (hook != NULL)
        hook(rs, &e);

    sync_atomic_bool_store(&rs->closing, true);
    for (SqlRows *c = rs->children; c != NULL; c = c->child_next)
        (void)sq_rows_close(c, rs->lasterr);
    if (rs->has_ctx)
        BURROW_CALLF0(rs->cancel);
    if (rs->close_stmt != NULL) {
        (void)sq_ds_close(rs->close_stmt);
        sq_ds_unref(rs->close_stmt);
        rs->close_stmt = NULL;
    }
    SqRelease rel = rs->release;
    rs->release = (SqRelease){SQ_RELEASE_NONE, NULL};
    sq_release(rel, e);
    if (rs->dep_stmt != NULL) {
        SqlStmt *s = rs->dep_stmt;
        rs->dep_stmt = NULL;
        (void)sq_stmt_remove_dep(s);
        sq_stmt_unref(s);
    }
    rs->lasterr = sq_rows_keep(rs, sq_rows_lasterr_or(rs, e));
    sq_closemu_unlock(&rs->closemu);
    return e;
}

static bool sq_rows_column_scanner(const SqlRows *rs) {
    return rs->rowsi.vt->next_row != NULL && rs->rowsi.vt->scan_column != NULL;
}

static bool sq_rows_next_result_setter(const SqlRows *rs) {
    return rs->rowsi.vt->has_next_result_set != NULL &&
           rs->rowsi.vt->next_result_set != NULL;
}

/* closemuRUnlockIfHeldByScan. */
static void sq_rows_unhold(SqlRows *rs) {
    if (rs->scan_hold) {
        rs->scan_hold = false;
        sq_closemu_runlock(&rs->closemu);
    }
}

/* nextLocked. */
static bool sq_rows_next_locked(SqlRows *rs, bool *do_close) {
    *do_close = false;
    if (rs->closed)
        return false;
    sync_mutex_lock(&rs->dc->mu);
    if (!rs->next_called) {
        rs->numcols = rs->rowsi.vt->columns(rs->rowsi.data).len;
        rs->next_called = true;
    }
    Error e = BURROW_NO_ERROR;
    if (sq_rows_column_scanner(rs)) {
        e = rs->rowsi.vt->next_row(rs->rowsi.data);
    } else {
        if (rs->lastcols == NULL) {
            Int n = rs->numcols > 0 ? rs->numcols : 1;
            rs->lastcols = BURROW_NEW_N(heap_allocator(), DriverValue, (size_t)n);
            if (rs->lastcols != NULL)
                rs->lastcols_len = n;
        }
        if (rs->lastcols == NULL)
            e = burrow_err_out_of_memory;
        else
            e = rs->rowsi.vt->next(rs->rowsi.data, slice_from(rs->lastcols, rs->numcols,
                                                              rs->numcols, TYPE_ANY));
    }
    rs->lasterr = sq_rows_keep(rs, e);
    bool ok = true;
    if (BURROW_FAILED(rs->lasterr)) {
        ok = false;
        if (!sq_same(rs->lasterr, io_eof) || !sq_rows_next_result_setter(rs))
            *do_close = true;
        else if (!rs->rowsi.vt->has_next_result_set(rs->rowsi.data))
            *do_close = true;
    }
    sync_mutex_unlock(&rs->dc->mu);
    return ok;
}

bool sql_rows_next(SqlRows *rs) {
    sq_rows_unhold(rs);
    /* If the user's calling Next, they're done with their previous row's Scan
     * results (any RawBytes memory), so we can release the read lock that
     * would be preventing awaitDone from calling close. */
    if (sync_atomic_bool_load(&rs->has_cd))
        return false;
    bool do_close = false;
    sq_closemu_rlock(&rs->closemu);
    bool ok = sq_rows_next_locked(rs, &do_close);
    sq_closemu_runlock(&rs->closemu);
    if (do_close)
        (void)sql_rows_close(rs);
    if (do_close && !ok)
        rs->hit_eof = true;
    return ok;
}

bool sql_rows_next_result_set(SqlRows *rs) {
    sq_rows_unhold(rs);
    bool do_close = false;
    bool ok = false;
    sq_closemu_rlock(&rs->closemu);
    if (!rs->closed) {
        rs->next_called = false;
        sq_rows_free_lastcols(rs);
        if (!sq_rows_next_result_setter(rs)) {
            do_close = true;
        } else {
            sync_mutex_lock(&rs->dc->mu);
            rs->lasterr =
                sq_rows_keep(rs, rs->rowsi.vt->next_result_set(rs->rowsi.data));
            sync_mutex_unlock(&rs->dc->mu);
            if (BURROW_FAILED(rs->lasterr))
                do_close = true;
            else
                ok = true;
        }
    }
    sq_closemu_runlock(&rs->closemu);
    if (do_close)
        (void)sql_rows_close(rs);
    return ok;
}

Error sql_rows_err(SqlRows *rs) {
    /* Return any context error that might've happened during row iteration,
     * but only if we haven't reported the final Next() = false after rows
     * are done, in which case the user might've gotten that error and be
     * done with it. */
    if (!rs->hit_eof && sync_atomic_bool_load(&rs->has_cd))
        return rs->context_done;
    sq_closemu_rlock(&rs->closemu);
    Error e = sq_rows_lasterr_or(rs, BURROW_NO_ERROR);
    sq_closemu_runlock(&rs->closemu);
    return e;
}

Slice sql_rows_columns(SqlRows *rs, Alloc *a, Error *err) {
    Slice out = slice_from(NULL, 0, 0, TYPE_STRING);
    sq_closemu_rlock(&rs->closemu);
    if (rs->closed) {
        BURROW_OUT(err, sq_rows_lasterr_or(rs, burrow__sql_err_rows_closed));
        sq_closemu_runlock(&rs->closemu);
        return out;
    }
    Error e = BURROW_NO_ERROR;
    sync_mutex_lock(&rs->dc->mu);
    Slice cols = rs->rowsi.vt->columns(rs->rowsi.data);
    Str *names = NULL;
    if (cols.len > 0) {
        names = BURROW_NEW_N(a, Str, (size_t)cols.len);
        if (names == NULL)
            e = burrow_err_out_of_memory;
    }
    const Str *src = (const Str *)cols.p;
    for (Int i = 0; !BURROW_FAILED(e) && i < cols.len; i++) {
        names[i] = BURROW_STR_EMPTY;
        if (src[i].len > 0) {
            names[i] = str_clone(a, src[i]);
            if (names[i].p == NULL)
                e = burrow_err_out_of_memory;
        }
    }
    sync_mutex_unlock(&rs->dc->mu);
    sq_closemu_runlock(&rs->closemu);
    BURROW_OUT(err, e);
    if (BURROW_FAILED(e))
        return out;
    return slice_from(names, cols.len, cols.len, TYPE_STRING);
}

/* rowsColumnInfoSetupConnLocked. */
static Error sq_column_info(Alloc *a, DriverRows rowsi, Slice *out) {
    const DriverRowsVT *vt = rowsi.vt;
    Slice names = vt->columns(rowsi.data);
    const Str *src = (const Str *)names.p;
    SqlColumnType **list = NULL;
    if (names.len > 0) {
        list = BURROW_NEW_N(a, SqlColumnType *, (size_t)names.len);
        if (list == NULL)
            return burrow_err_out_of_memory;
    }
    for (Int i = 0; i < names.len; i++) {
        SqlColumnType *ci = BURROW_NEW(a, SqlColumnType);
        if (ci == NULL)
            return burrow_err_out_of_memory;
        list[i] = ci;
        ci->name = BURROW_STR_EMPTY;
        ci->database_type = BURROW_STR_EMPTY;
        if (src[i].len > 0) {
            ci->name = str_clone(a, src[i]);
            if (ci->name.p == NULL)
                return burrow_err_out_of_memory;
        }
        ci->scan_type = TYPE_ANY;
        if (vt->column_type_scan_type != NULL)
            ci->scan_type = vt->column_type_scan_type(rowsi.data, i);
        if (vt->column_type_database_type_name != NULL) {
            Str t = vt->column_type_database_type_name(rowsi.data, i);
            if (t.len > 0) {
                ci->database_type = str_clone(a, t);
                if (ci->database_type.p == NULL)
                    return burrow_err_out_of_memory;
            }
        }
        if (vt->column_type_length != NULL)
            ci->length = vt->column_type_length(rowsi.data, i, &ci->has_length);
        if (vt->column_type_nullable != NULL)
            ci->nullable = vt->column_type_nullable(rowsi.data, i, &ci->has_nullable);
        if (vt->column_type_precision_scale != NULL)
            ci->precision = vt->column_type_precision_scale(rowsi.data, i, &ci->scale,
                                                            &ci->has_precision_scale);
    }
    *out = slice_from(list, names.len, names.len, TYPE_UNSAFE_POINTER);
    return BURROW_NO_ERROR;
}

Slice sql_rows_column_types(SqlRows *rs, Alloc *a, Error *err) {
    Slice out = slice_from(NULL, 0, 0, TYPE_UNSAFE_POINTER);
    sq_closemu_rlock(&rs->closemu);
    if (rs->closed) {
        BURROW_OUT(err, sq_rows_lasterr_or(rs, burrow__sql_err_rows_closed));
        sq_closemu_runlock(&rs->closemu);
        return out;
    }
    sync_mutex_lock(&rs->dc->mu);
    Slice list = out;
    Error e = sq_column_info(a, rs->rowsi, &list);
    sync_mutex_unlock(&rs->dc->mu);
    sq_closemu_runlock(&rs->closemu);
    BURROW_OUT(err, e);
    return BURROW_FAILED(e) ? out : list;
}

static Str sq_rows_column_name(const SqlRows *rs, Int i) {
    Slice cols = rs->rowsi.vt->columns(rs->rowsi.data);
    if (i < 0 || i >= cols.len)
        return BURROW_STR_EMPTY;
    return ((const Str *)cols.p)[i];
}

/* scanLocked. */
static Error sq_rows_scan_locked(SqlRows *rs, Alloc *a, Slice dest) {
    if (BURROW_FAILED(rs->lasterr) && !sq_same(rs->lasterr, io_eof))
        return rs->lasterr;
    if (rs->closed)
        return sq_rows_lasterr_or(rs, burrow__sql_err_rows_closed);
    if (!rs->next_called)
        return sq_new_error(BURROW_S("sql: Scan called without calling Next"));
    if (dest.len != rs->numcols)
        return fmt_errorf_v("sql: expected %d destination arguments in Scan, not %d",
                            rs->numcols, dest.len);
    const Any *d = (const Any *)dest.p;
    if (sq_rows_column_scanner(rs)) {
        rs->scan_alloc = a;
        Error out = BURROW_NO_ERROR;
        sync_mutex_lock(&rs->dc->mu);
        for (Int i = 0; i < dest.len; i++) {
            DriverScanContext scan_ctx = {{&burrow__sql_rows_type, rs}};
            Error e = rs->rowsi.vt->scan_column(rs->rowsi.data, scan_ctx, i, d[i]);
            if (BURROW_FAILED(e)) {
                out = fmt_errorf_v("sql: Scan error on column index %d, name %q: %w", i,
                                   sq_rows_column_name(rs, i), e);
                break;
            }
        }
        sync_mutex_unlock(&rs->dc->mu);
        rs->scan_alloc = NULL;
        return out;
    }
    for (Int i = 0; i < rs->numcols; i++) {
        Error e = burrow__sql_convert_assign_rows(a, d[i], rs->lastcols[i], rs);
        if (BURROW_FAILED(e))
            return fmt_errorf_v("sql: Scan error on column index %d, name %q: %w", i,
                                sq_rows_column_name(rs, i), e);
    }
    return BURROW_NO_ERROR;
}

static bool sq_args_have_raw_bytes(Slice dest) {
    const Any *d = (const Any *)dest.p;
    for (Int i = 0; i < dest.len; i++) {
        if (d[i].t == &burrow_type_SqlRawBytes)
            return true;
    }
    return false;
}

Error sql_rows_scan(SqlRows *rs, Alloc *a, Slice dest) {
    if (rs->scan_hold)
        return sq_new_error(
            BURROW_S("sql: Scan called without calling Next (closemuScanHold)"));
    sq_closemu_rlock(&rs->closemu);
    rs->raw.len = 0;
    Error err = sq_rows_scan_locked(rs, a, dest);
    if (!BURROW_FAILED(err) && sq_args_have_raw_bytes(dest))
        rs->scan_hold = true;
    else
        sq_closemu_runlock(&rs->closemu);
    return err;
}

Error sql_rows_close(SqlRows *rs) {
    /* If the user's calling Close, they're done with their previous row's
     * Scan results (any RawBytes memory), so we can release the read lock that
     * would be preventing awaitDone from calling the Rows' close. */
    sq_rows_unhold(rs);
    return sq_rows_close(rs, BURROW_NO_ERROR);
}

void sql_rows_free(SqlRows *rs) {
    if (rs == NULL)
        return;
    (void)sql_rows_close(rs);
    sq_rows_unref(rs);
}

/* -------------------------------------------------------------------- Row */

Error sql_row_scan(SqlRow *r, Alloc *a, Slice dest) {
    if (BURROW_FAILED(r->err))
        return r->err;
    SqlRows *rs = r->rows;
    if (rs == NULL)
        return sql_err_no_rows;
    Error e = BURROW_NO_ERROR;
    if (sq_args_have_raw_bytes(dest)) {
        e = sq_new_error(BURROW_S("sql: RawBytes isn't allowed on Row.Scan"));
    } else if (!sql_rows_next(rs)) {
        e = sql_rows_err(rs);
        if (!BURROW_FAILED(e))
            e = sql_err_no_rows;
    } else {
        e = sql_rows_scan(rs, a, dest);
        if (!BURROW_FAILED(e))
            e = sql_rows_close(rs);
    }
    /* The error may live in the Rows, which goes now. */
    if (BURROW_FAILED(e))
        e = error_retain(error_allocator(), e);
    sql_rows_free(rs);
    r->rows = NULL;
    return e;
}

Error sql_row_err(const SqlRow *r) {
    return r->err;
}

/* ------------------------------------------------------------- ColumnType */

Str sql_column_type_name(const SqlColumnType *ct) {
    return ct->name;
}

int64_t sql_column_type_length(const SqlColumnType *ct, bool *ok) {
    if (ok != NULL)
        *ok = ct->has_length;
    return ct->length;
}

int64_t sql_column_type_decimal_size(const SqlColumnType *ct, int64_t *scale,
                                     bool *ok) {
    if (scale != NULL)
        *scale = ct->scale;
    if (ok != NULL)
        *ok = ct->has_precision_scale;
    return ct->precision;
}

const Type *sql_column_type_scan_type(const SqlColumnType *ct) {
    return ct->scan_type;
}

bool sql_column_type_nullable(const SqlColumnType *ct, bool *ok) {
    if (ok != NULL)
        *ok = ct->has_nullable;
    return ct->nullable;
}

Str sql_column_type_database_type_name(const SqlColumnType *ct) {
    return ct->database_type;
}

/* --------------------------------------------- what sql_convert.c calls */

Bytes burrow__sql_rows_rawbuf(SqlRows *rs) {
    if (rs == NULL)
        return slice_from(NULL, 0, 0, TYPE_BYTE);
    return rs->raw;
}

Bytes burrow__sql_rows_setrawbuf(SqlRows *rs, Bytes b) {
    if (rs == NULL)
        return b;
    Int off = rs->raw.len;
    rs->raw = b;
    if (b.p == NULL)
        return b;
    return slice_from((Byte *)b.p + off, b.len - off, b.cap - off, TYPE_BYTE);
}

Alloc *burrow__sql_rows_scan_alloc(SqlRows *rs) {
    return rs->scan_alloc;
}

Alloc *burrow__sql_rows_raw_alloc(SqlRows *rs, Alloc *a) {
    if (rs == NULL)
        return a;
    return arena_allocator(&rs->raw_arena);
}

SqlRows *burrow__sql_rows_cursor(SqlRows *parent, DriverRows rows, Error *err) {
    SqlRows *rs = sq_rows_new(parent->dc, (SqRelease){SQ_RELEASE_NONE, NULL}, rows);
    if (rs == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    /* One reference for the caller and one for parent, which closes it. */
    sync_atomic_int64_store(&rs->refs, 2);
    rs->child_next = parent->children;
    parent->children = rs;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return rs;
}

/* ------------------------------------------------------- for the tests */

Int burrow__sql_db_num_open(SqlDB *db) {
    sync_mutex_lock(&db->mu);
    Int n = db->num_open;
    sync_mutex_unlock(&db->mu);
    return n;
}

Int burrow__sql_db_num_free(SqlDB *db) {
    sync_mutex_lock(&db->mu);
    Int n = db->free_conn.len;
    sync_mutex_unlock(&db->mu);
    return n;
}

DriverConn burrow__sql_db_free_conn(SqlDB *db, Int i) {
    sync_mutex_lock(&db->mu);
    DriverConn ci = {NULL, NULL};
    if (i >= 0 && i < db->free_conn.len)
        ci = ((SqDC *)db->free_conn.p[i])->ci;
    sync_mutex_unlock(&db->mu);
    return ci;
}

Int burrow__sql_db_num_waiting(SqlDB *db) {
    sync_mutex_lock(&db->mu);
    Int n = db->reqs.len;
    sync_mutex_unlock(&db->mu);
    return n;
}

Int burrow__sql_db_num_deps(SqlDB *db) {
    sync_mutex_lock(&db->mu);
    Int n = db->num_dep;
    sync_mutex_unlock(&db->mu);
    return n;
}

void *burrow__sql_db_conn(SqlDB *db, Context ctx, bool always_new, Error *err) {
    return sq_conn(db, ctx, always_new ? SQ_ALWAYS_NEW : SQ_CACHED_OR_NEW, err);
}

void burrow__sql_db_put_conn(void *dc, Error err, bool reset_session) {
    sq_put_conn(dc, err, reset_session);
}

DriverConn burrow__sql_dc_driver(void *dc) {
    return ((SqDC *)dc)->ci;
}

void burrow__sql_stmt_force_prepare(SqlStmt *s) {
    sync_mutex_lock(&s->mu);
    for (Int i = 0; i < s->css.len; i++)
        sq_ds_unref((SqDS *)s->css.p[i]);
    s->css.len = 0;
    sync_mutex_unlock(&s->mu);
}

void *burrow__sql_db_free_dc(SqlDB *db, Int i) {
    SqDC *dc = NULL;
    sync_mutex_lock(&db->mu);
    if (i >= 0 && i < db->free_conn.len) {
        dc = db->free_conn.p[i];
        sync_atomic_int64_add(&dc->refs, 1);
    }
    sync_mutex_unlock(&db->mu);
    return dc;
}

void burrow__sql_dc_unref(void *dc) {
    sq_dc_unref(dc);
}

bool burrow__sql_dc_closed(void *p) {
    SqDC *dc = p;
    sync_mutex_lock(&dc->mu);
    bool closed = dc->closed;
    sync_mutex_unlock(&dc->mu);
    return closed;
}

Int burrow__sql_dc_num_open_stmt(void *p) {
    SqDC *dc = p;
    Int n = 0;
    sync_mutex_lock(&dc->mu);
    for (SqDS *ds = dc->open_stmt; ds != NULL; ds = ds->os_next)
        n++;
    sync_mutex_unlock(&dc->mu);
    return n;
}

bool burrow__sql_dc_has_driver(void *p) {
    SqDC *dc = p;
    sync_mutex_lock(&dc->mu);
    bool has = dc->ci.vt != NULL;
    sync_mutex_unlock(&dc->mu);
    return has;
}

int64_t burrow__sql_db_num_closed(SqlDB *db) {
    return sync_atomic_int64_load(&db->num_closed);
}

Int burrow__sql_stmt_num_css(SqlStmt *s) {
    sync_mutex_lock(&s->mu);
    Int n = s->css.len;
    sync_mutex_unlock(&s->mu);
    return n;
}

DriverConn burrow__sql_conn_driver(SqlConn *c) {
    return c->dc != NULL ? c->dc->ci : (DriverConn){NULL, NULL};
}

DriverConn burrow__sql_tx_driver(SqlTx *tx) {
    return tx->dc != NULL ? tx->dc->ci : (DriverConn){NULL, NULL};
}

DriverRows burrow__sql_rows_driver(SqlRows *rs) {
    return rs->rowsi;
}

bool burrow__sql_tx_is_done(SqlTx *tx) {
    return sync_atomic_bool_load(&tx->done);
}

void *burrow__sql_rows_dc(SqlRows *rs) {
    return rs->dc;
}

Duration burrow__sql_db_cleaner_run(SqlDB *db, Duration d, Int *nclosed,
                                    bool *ordered) {
    SqDC *closing = NULL;
    sync_mutex_lock(&db->mu);
    d = sq_cleaner_run_locked(db, d, &closing);
    *ordered = true;
    Time last = {0};
    for (Int i = 0; i < db->free_conn.len; i++) {
        SqDC *c = (SqDC *)db->free_conn.p[i];
        if (time_after(last, c->returned_at)) {
            *ordered = false;
            break;
        }
        last = c->returned_at;
    }
    sync_mutex_unlock(&db->mu);
    Int n = 0;
    for (SqDC *c = closing; c != NULL; c = c->close_next)
        n++;
    sq_close_list(closing);
    *nclosed = n;
    return d;
}

Int burrow__sql_db_free_conn_open_stmts(SqlDB *db, Int i) {
    Int n = 0;
    sync_mutex_lock(&db->mu);
    if (i >= 0 && i < db->free_conn.len) {
        SqDC *dc = db->free_conn.p[i];
        sync_mutex_lock(&dc->mu);
        for (SqDS *ds = dc->open_stmt; ds != NULL; ds = ds->os_next)
            n++;
        sync_mutex_unlock(&dc->mu);
    }
    sync_mutex_unlock(&db->mu);
    return n;
}

bool burrow__sql_rows_closed(SqlRows *rs) {
    sq_closemu_rlock(&rs->closemu);
    bool closed = rs->closed;
    sq_closemu_runlock(&rs->closemu);
    return closed;
}

void burrow__sql_tx_set_keep_conn_on_rollback(SqlTx *tx, bool keep) {
    tx->keep_conn_on_rollback = keep;
}

bool burrow__sql_stmt_closed(SqlStmt *s) {
    sync_mutex_lock(&s->mu);
    bool closed = s->closed;
    sync_mutex_unlock(&s->mu);
    return closed;
}

Error burrow__sql_stmt_sticky_err(SqlStmt *s) {
    return s->sticky;
}

SqlStmt *burrow__sql_stmt_parent(SqlStmt *s) {
    return s->parent;
}
