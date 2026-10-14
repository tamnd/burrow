#include <string.h>

#include "burrow/burrow.h"

/* memdb, a driver that keeps one table of people in memory and understands
 * three statements. A real driver talks to a server, but the shape is the
 * same: the Driver opens a Conn, the Conn prepares a Stmt, and the Stmt runs
 * and hands back a Result or Rows. */

#define INSERT_PERSON "INSERT INTO people (name, age) VALUES (?, ?)"
#define SELECT_PEOPLE "SELECT name, age FROM people"
#define SELECT_AGE "SELECT age FROM people WHERE name = ?"

typedef struct Person {
    char name[32];
    Int name_len;
    int64_t age;
} Person;

/* The table, shared by every connection. */
static SyncMutex table_mu;
static Person table[16];
static Int table_len;

typedef enum MemQuery { MEM_INSERT, MEM_SELECT_PEOPLE, MEM_SELECT_AGE } MemQuery;

typedef struct MemConn {
    bool in_tx;
    Int tx_start; /* how many rows the table had when the transaction began */
} MemConn;

typedef struct MemStmt {
    MemQuery q;
} MemStmt;

/* A snapshot of the rows a query matched, so the table can change while the
 * caller reads them. */
typedef struct MemRows {
    Person rows[16];
    Int len, pos;
    Str cols[2];
    Int ncols;
    Str name; /* the current row's name, which next hands out */
} MemRows;

static Error mem_rows_close(void *self) {
    mem_free(heap_allocator(), self, sizeof(MemRows), _Alignof(MemRows));
    return BURROW_NO_ERROR;
}

static Slice mem_rows_columns(void *self) {
    MemRows *r = self;
    return slice_from(r->cols, r->ncols, r->ncols, TYPE_STRING);
}

/* What dest gets has to stay good until the next call to next, which a field
 * of the snapshot does. */
static Error mem_rows_next(void *self, Slice dest) {
    MemRows *r = self;
    if (r->pos == r->len)
        return io_eof;
    Person *p = &r->rows[r->pos++];
    Any *d = dest.p;
    if (r->ncols == 1) {
        d[0] = (Any){TYPE_INT64, &p->age};
        return BURROW_NO_ERROR;
    }
    r->name = (Str){(const Byte *)p->name, p->name_len};
    d[0] = (Any){TYPE_STRING, &r->name};
    d[1] = (Any){TYPE_INT64, &p->age};
    return BURROW_NO_ERROR;
}

static const DriverRowsVT mem_rows_vt = {
    .close = mem_rows_close,
    .columns = mem_rows_columns,
    .next = mem_rows_next,
};

static Error mem_stmt_close(void *self) {
    mem_free(heap_allocator(), self, sizeof(MemStmt), _Alignof(MemStmt));
    return BURROW_NO_ERROR;
}

/* How many arguments each statement takes. database/sql checks the count
 * before it calls exec or query. */
static Int mem_stmt_num_input(void *self) {
    MemStmt *s = self;
    return s->q == MEM_SELECT_PEOPLE ? 0 : s->q == MEM_INSERT ? 2 : 1;
}

/* The arguments arrive already converted by driver_default_parameter_converter,
 * so a name is a Str and an age is an int64_t. */
static DriverResult mem_stmt_exec(void *self, Alloc *a, Slice args, Error *err) {
    MemStmt *s = self;
    const Any *v = args.p;
    if (s->q != MEM_INSERT) {
        *err = fmt_errorf_v("memdb: exec of a query");
        return (DriverResult){NULL, NULL};
    }
    Str name = *(const Str *)v[0].data;
    if (name.len > 31) {
        *err = fmt_errorf_v("memdb: name too long");
        return (DriverResult){NULL, NULL};
    }
    sync_mutex_lock(&table_mu);
    if (table_len == 16) {
        sync_mutex_unlock(&table_mu);
        *err = fmt_errorf_v("memdb: table full");
        return (DriverResult){NULL, NULL};
    }
    Person *p = &table[table_len++];
    memcpy(p->name, name.p, (size_t)name.len);
    p->name_len = name.len;
    p->age = *(const int64_t *)v[1].data;
    sync_mutex_unlock(&table_mu);

    DriverRowsAffected *n = BURROW_NEW(a, DriverRowsAffected);
    if (n == NULL) {
        *err = burrow_err_out_of_memory;
        return (DriverResult){NULL, NULL};
    }
    *n = 1;
    return driver_rows_affected_as_result(n);
}

static DriverRows mem_stmt_query(void *self, Slice args, Error *err) {
    MemStmt *s = self;
    if (s->q == MEM_INSERT) {
        *err = fmt_errorf_v("memdb: query of an insert");
        return (DriverRows){NULL, NULL};
    }
    MemRows *r = BURROW_NEW(heap_allocator(), MemRows);
    if (r == NULL) {
        *err = burrow_err_out_of_memory;
        return (DriverRows){NULL, NULL};
    }
    Str want = BURROW_STR_EMPTY;
    if (s->q == MEM_SELECT_AGE) {
        want = *(const Str *)((const Any *)args.p)[0].data;
        r->cols[0] = BURROW_S("age");
        r->ncols = 1;
    } else {
        r->cols[0] = BURROW_S("name");
        r->cols[1] = BURROW_S("age");
        r->ncols = 2;
    }
    sync_mutex_lock(&table_mu);
    for (Int i = 0; i < table_len; i++) {
        Str name = {(const Byte *)table[i].name, table[i].name_len};
        if (s->q == MEM_SELECT_PEOPLE || str_eq(name, want))
            r->rows[r->len++] = table[i];
    }
    sync_mutex_unlock(&table_mu);
    return (DriverRows){&mem_rows_vt, r};
}

static const DriverStmtVT mem_stmt_vt = {
    .close = mem_stmt_close,
    .num_input = mem_stmt_num_input,
    .exec = mem_stmt_exec,
    .query = mem_stmt_query,
};

static DriverStmt mem_conn_prepare(void *self, Str query, Error *err) {
    (void)self;
    MemQuery q;
    if (str_eq(query, BURROW_S(INSERT_PERSON))) {
        q = MEM_INSERT;
    } else if (str_eq(query, BURROW_S(SELECT_PEOPLE))) {
        q = MEM_SELECT_PEOPLE;
    } else if (str_eq(query, BURROW_S(SELECT_AGE))) {
        q = MEM_SELECT_AGE;
    } else {
        *err = fmt_errorf_v("memdb: cannot run %q", query);
        return (DriverStmt){NULL, NULL};
    }
    MemStmt *s = BURROW_NEW(heap_allocator(), MemStmt);
    if (s == NULL) {
        *err = burrow_err_out_of_memory;
        return (DriverStmt){NULL, NULL};
    }
    s->q = q;
    return (DriverStmt){&mem_stmt_vt, s};
}

/* A transaction remembers where the table ended, and a rollback cuts it back
 * there. Good enough for one writer at a time. */
static Error mem_tx_commit(void *self) {
    MemConn *c = self;
    c->in_tx = false;
    return BURROW_NO_ERROR;
}

static Error mem_tx_rollback(void *self) {
    MemConn *c = self;
    sync_mutex_lock(&table_mu);
    table_len = c->tx_start;
    sync_mutex_unlock(&table_mu);
    c->in_tx = false;
    return BURROW_NO_ERROR;
}

static const DriverTxVT mem_tx_vt = {
    .commit = mem_tx_commit,
    .rollback = mem_tx_rollback,
};

static DriverTx mem_conn_begin(void *self, Error *err) {
    MemConn *c = self;
    if (c->in_tx) {
        *err = fmt_errorf_v("memdb: already in a transaction");
        return (DriverTx){NULL, NULL};
    }
    sync_mutex_lock(&table_mu);
    c->tx_start = table_len;
    sync_mutex_unlock(&table_mu);
    c->in_tx = true;
    return (DriverTx){&mem_tx_vt, c};
}

static Error mem_conn_close(void *self) {
    mem_free(heap_allocator(), self, sizeof(MemConn), _Alignof(MemConn));
    return BURROW_NO_ERROR;
}

static const DriverConnVT mem_conn_vt = {
    .prepare = mem_conn_prepare,
    .close = mem_conn_close,
    .begin = mem_conn_begin,
};

static DriverConn mem_open(void *self, Str name, Error *err) {
    (void)self;
    (void)name;
    MemConn *c = BURROW_NEW(heap_allocator(), MemConn);
    if (c == NULL) {
        *err = burrow_err_out_of_memory;
        return (DriverConn){NULL, NULL};
    }
    return (DriverConn){&mem_conn_vt, c};
}

static const DriverVT mem_driver_vt = {.open = mem_open};

// doc: count
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
// doc: end

static void run(void *env) {
    (void)env;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);

    // doc: open
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
    // doc: end

    // doc: exec
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
    // doc: end

    list_people(db, a);

    // doc: queryrow
    int64_t age = 0;
    SqlRow row = sql_db_query_row_v(db, BURROW_S(SELECT_AGE), BURROW_S("Grace"));
    err = sql_row_scan_v(&row, a, &age);
    fmt_printf_v("Grace: %d, %v\n", age, err);

    row = sql_db_query_row_v(db, BURROW_S(SELECT_AGE), BURROW_S("Alan"));
    err = sql_row_scan_v(&row, a, &age);
    if (errors_is(err, sql_err_no_rows))
        fmt_printf_v("Alan: no such person\n");
    // doc: end

    // doc: prepare
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
    // doc: end

    // doc: tx
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
    // doc: end

    // doc: bad
    (void)sql_db_exec_v(db, a, &err, BURROW_S(INSERT_PERSON), BURROW_S("Barbara"));
    fmt_printf_v("one argument: %v\n", err);
    (void)sql_db_exec(db, a, BURROW_S("DROP TABLE people"), (Slice){0}, &err);
    fmt_printf_v("unknown statement: %v\n", err);
    // doc: end

    // doc: close
    err = sql_db_close(db);
    fmt_printf_v("close: %v\n", err);
    sql_db_free(db);
    // doc: end

    arena_free(&ar);
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}

/* Output:
ping: <nil>
inserted Ada, 1 row
inserted Grace, 1 row
inserted Edsger, 1 row
Ada is 36
Grace is 85
Edsger is 72
3 people
Grace: 85, <nil>
Alan: no such person
Edsger: 72
Ada: 36
insert in tx: <nil>
rollback: <nil>
commit after rollback: sql: transaction has already been committed or rolled back
Ada is 36
Grace is 85
Edsger is 72
3 people
one argument: sql: expected 2 arguments, got 1
unknown statement: memdb: cannot run "DROP TABLE people"
close: <nil>
*/
