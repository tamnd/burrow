/* database/sql/driver, the interfaces a database driver implements so that
 * database/sql can use it.
 *
 * Most code never includes this. It is for the people writing a driver, and
 * for programs that want a driver's optional features through database/sql.
 *
 * Each of Go's interfaces here is a vtable and a data pointer, the same as
 * io's. Go has a dozen small optional interfaces that a Conn, a Stmt or a Rows
 * may also implement, and database/sql asks for each with a type assertion. In
 * C those are optional slots in the one vtable instead, NULL when the driver
 * does not have the method, so a driver fills in one table per type:
 *
 *     static const DriverConnVT memconn_vt = {
 *         .self_type = &memconn_type,
 *         .prepare = memconn_prepare,
 *         .close = memconn_close,
 *         .begin = memconn_begin,
 *         .exec_context = memconn_exec_context,
 *         .query_context = memconn_query_context,
 *         .reset_session = memconn_reset_session,
 *     };
 *
 * The type names Go gives the optional interfaces are still here, as other
 * names for the vtable they are part of: DriverPinger is a DriverConn, and a
 * DriverPinger's vtable is a DriverConnVT whose ping is set.
 *
 * A DriverValue is an Any holding one of the types Go allows: nothing,
 * int64_t, double, bool, Bytes, Str or Time, or a decimal, which is any type
 * with a Decompose method. See driver_is_value.
 *
 * MEMORY
 *
 * Go's collector decides how long a driver's objects live, and here the
 * methods that end them say so. A Conn lives until its close, a Stmt until its
 * close, a Rows until its close and a Tx until its commit or rollback, and
 * database/sql calls each of those exactly once and then forgets the object.
 * A Result has no method that ends it, so the exec slots take an allocator and
 * the Result, with anything it points to, comes from that. So does a converted
 * value, from the allocator convert_value is given.
 *
 * Strings and slices passed to a method are the caller's and are only good
 * until the method returns, so a driver copies anything it keeps. What a Rows
 * gives back, its column names, its type names and the values next writes into
 * dest, belongs to the Rows. Names and type names are good until close, and
 * values until the next call to next or close, which is what Go's Rows.Next
 * says about a buffer it puts in dest.
 *
 * Errors are burrow errors, and driver_err_bad_conn and driver_err_skip mean
 * what they do in Go: database/sql compares against them with errors_is.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package database/sql/driver */

#ifndef BURROW_DATABASE_SQL_DRIVER_H
#define BURROW_DATABASE_SQL_DRIVER_H

#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/iface.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* driver.Value. The nil Any is SQL's NULL. */
typedef Any DriverValue;

/* driver.NamedValue, an argument with its name and its position. name is
 * empty for an argument without one. ordinal starts at 1, and is set whether
 * or not the argument has a name. */
typedef struct DriverNamedValue {
    Str name;
    Int ordinal;
    DriverValue value;
} DriverNamedValue;

extern const Type burrow_type_DriverNamedValue;
#define TYPE_DRIVER_NAMED_VALUE TYPE_OF(DriverNamedValue)

/* driver.ErrSkip. Returned by an optional slot to mean "do what you would do if
 * this slot were NULL". Only the slots that say so may return it. */
extern const Error driver_err_skip;

/* driver.ErrBadConn. Tells database/sql that the connection is no good and it
 * should try again on a new one. Return it only when the server cannot have
 * done the work, since database/sql will do it again. A driver that is not
 * sure returns some other error. */
extern const Error driver_err_bad_conn;

/* driver.ErrRemoveArgument. check_named_value returns it to drop an argument
 * from the list instead of passing it to the query. */
extern const Error driver_err_remove_argument;

/* driver.IsolationLevel, the one in DriverTxOptions. database/sql's levels
 * convert to it. */
typedef Int DriverIsolationLevel;

/* driver.TxOptions. Isolation 0 is the driver's default. */
typedef struct DriverTxOptions {
    DriverIsolationLevel isolation;
    bool read_only;
} DriverTxOptions;

/* driver.ScanContext. What database/sql passes through scan_column, for the
 * driver to hand to sql_convert_assign unopened. Only database/sql looks
 * inside. */
typedef struct DriverScanContext {
    Any burrow__v;
} DriverScanContext;

/* ------------------------------------------------------------------ Result */

/* driver.Result, what an exec returns. Comes from the allocator the exec was
 * given. */
typedef struct DriverResultVT {
    const Type *self_type;
    /* The id the database made for the row this inserted, an auto increment
     * column for instance. */
    int64_t (*last_insert_id)(void *self, Error *err);
    /* How many rows this changed. */
    int64_t (*rows_affected)(void *self, Error *err);
} DriverResultVT;

typedef struct DriverResult {
    const DriverResultVT *vt;
    void *data;
} DriverResult;

/* driver.RowsAffected, a Result that has a row count and no insert id. */
typedef int64_t DriverRowsAffected;

/* RowsAffected.LastInsertId, which always fails. */
int64_t driver_rows_affected_last_insert_id(DriverRowsAffected v, Error *err);
int64_t driver_rows_affected_rows_affected(DriverRowsAffected v, Error *err);

/* v as a Result. The Result points at v, so v has to outlive it, which in an
 * exec means allocating v from the allocator the exec was given. */
BURROW_BORROWS(ret, v) DriverResult
driver_rows_affected_as_result(DriverRowsAffected *v);

/* driver.ResultNoRows, for a statement such as CREATE TABLE that has neither
 * an insert id nor a row count. Both of its methods fail. */
extern const DriverResult driver_result_no_rows;

/* ---------------------------------------------------------------------- Tx */

/* driver.Tx, a transaction. Ends with one call to commit or rollback, after
 * which the driver can free it. */
typedef struct DriverTxVT {
    const Type *self_type;
    Error (*commit)(void *self);
    Error (*rollback)(void *self);
} DriverTxVT;

typedef struct DriverTx {
    const DriverTxVT *vt;
    void *data;
} DriverTx;

/* ---------------------------------------------------------- ValueConverter */

/* driver.ValueConverter, which turns a value into a DriverValue. The result
 * is v itself, or comes from a and may still point into v's bytes or string, so
 * it lives as long as both. */
typedef struct DriverValueConverterVT {
    const Type *self_type;
    DriverValue (*convert_value)(void *self, Alloc *a, Any v, Error *err);
} DriverValueConverterVT;

typedef struct DriverValueConverter {
    const DriverValueConverterVT *vt;
    void *data;
} DriverValueConverter;

/* driver.Bool: true and false as they are, "1", "t", "true", "0", "f",
 * "false" and the other spellings strconv_parse_bool takes, as text or bytes,
 * and the integers 1 and 0. */
extern const DriverValueConverter driver_bool;

/* driver.Int32: any integer or integer text in the range of an int32_t, as an
 * int64_t. */
extern const DriverValueConverter driver_int32;

/* driver.String: Str and Bytes as they are, and anything else printed with
 * fmt's %v. */
extern const DriverValueConverter driver_string;

/* driver.DefaultParameterConverter, what database/sql does with an argument
 * when neither the statement nor the connection says otherwise. A value that
 * already is a DriverValue stays as it is. A Valuer is asked for its value. A
 * pointer is followed, NULL being SQL's NULL. Integers of every width and
 * signedness become int64_t, unless they do not fit, floats become double,
 * any bool, string or byte slice type becomes the plain one, and a Uuid
 * becomes its text. Anything else is an error. */
extern const DriverValueConverter driver_default_parameter_converter;

/* driver.Null, a converter that lets the nil Any through and gives anything
 * else to converter. */
typedef struct DriverNull {
    DriverValueConverter converter;
} DriverNull;

BURROW_OWNS(ret) BURROW_BORROWS(ret, v) DriverValue
driver_null_convert_value(DriverNull n, Alloc *a, Any v, Error *err);
/* n as a ValueConverter. Points at n, which has to outlive it. */
BURROW_BORROWS(ret, n) DriverValueConverter
driver_null_as_value_converter(DriverNull *n);

/* driver.NotNull, a converter that refuses the nil Any and gives anything
 * else to converter. */
typedef struct DriverNotNull {
    DriverValueConverter converter;
} DriverNotNull;

BURROW_OWNS(ret) BURROW_BORROWS(ret, v) DriverValue
driver_not_null_convert_value(DriverNotNull n, Alloc *a, Any v, Error *err);
BURROW_BORROWS(ret, n) DriverValueConverter
driver_not_null_as_value_converter(DriverNotNull *n);

/* driver.IsValue: whether v is a DriverValue, which is the nil Any, an
 * int64_t, a double, a bool, Bytes, a Str, a Time, or a value with a Decompose
 * method. */
bool driver_is_value(Any v);

/* driver.IsScanValue, the same as driver_is_value, as in Go. */
bool driver_is_scan_value(Any v);

/* ------------------------------------------------------------------ Valuer
 *
 * driver.Valuer, a type of the program's own that knows how to turn itself
 * into a DriverValue: a money type that is stored as a string, say. Like
 * encoding's interfaces, a type implements it by listing a Value method in its
 * descriptor with the signature below, and database/sql finds it on an
 * argument it is handed as an Any:
 *
 *     static DriverValue money_value(Money *m, Alloc *a, Error *err) {
 *         BURROW_OUT(err, BURROW_NO_ERROR);
 *         Str *s = BURROW_NEW(a, Str);
 *         if (s == NULL) { ... }
 *         *s = fmt_sprintf_v(a, "%d.%02d", m->cents / 100, m->cents % 100);
 *         return (DriverValue){TYPE_STRING, s};
 *     }
 *
 *     #define MONEY_METHODS(M, T) M(T, Value, money_value, DRIVER_SIG_VALUE)
 *     BURROW_STRUCT_DEFINE_METHODS(Money, MONEY_FIELDS, MONEY_METHODS);
 *
 * An Any whose type is TYPE_OF(DriverValuer), holding the interface value
 * itself, works as well. As in Go, a NULL pointer to a type whose Value method
 * takes the value is SQL's NULL rather than a call with a NULL receiver. */
typedef struct DriverValuerVT {
    const Type *self_type;
    DriverValue (*value)(void *self, Alloc *a, Error *err);
} DriverValuerVT;

typedef struct DriverValuer {
    const DriverValuerVT *vt;
    void *data;
} DriverValuer;

extern const Type burrow_type_DriverValuer;

/* Alloc * and Error * under one word each, since a signature list names its
 * types by a single token. */
typedef Alloc *DriverAllocArg;
typedef Error *DriverErrorArg;
extern const Type burrow_type_DriverAllocArg;
extern const Type burrow_type_DriverErrorArg;

#define DRIVER_SIG_VALUE(IN, OUT) IN(0, DriverAllocArg) IN(1, DriverErrorArg) OUT(Any)

/* A decimal, which Go recognises by its Decompose method:
 *
 *     Decompose(buf []byte) (form byte, negative bool, coefficient []byte,
 *                            exponent int32)
 *
 * form is 0 for a finite number, 1 for infinity and 2 for NaN. A finite one is
 * coefficient, a big endian unsigned integer, times ten to the exponent,
 * negated when negative is true. buf is somewhere the method may put the
 * coefficient if it is big enough. Here the four results are this struct, and
 * the method goes in a type's descriptor with DRIVER_SIG_DECOMPOSE. */
typedef struct DriverDecomposed {
    Byte form;
    bool negative;
    Bytes coefficient;
    int32_t exponent;
} DriverDecomposed;

extern const Type burrow_type_DriverDecomposed;

#define DRIVER_SIG_DECOMPOSE(IN, OUT) IN(0, Bytes) OUT(DriverDecomposed)

/* -------------------------------------------------------------------- Rows */

/* driver.Rows, the result of a query, read one row at a time. close is called
 * once and the driver frees it there. */
typedef struct DriverRowsVT {
    const Type *self_type;
    /* The column names, a Slice of Str, as many as there are columns. A name
     * that is not known is "". Belongs to the Rows. */
    Slice (*columns)(void *self);
    Error (*close)(void *self);
    /* Fills dest, a Slice of DriverValue as long as columns, with the next
     * row, and returns io_eof when there are no more. The values belong to the
     * Rows and are good until the next call to next or close. */
    Error (*next)(void *self, Slice dest);

    /* RowsColumnScanner. Set both or neither. When they are set, database/sql
     * calls next_row to move to the next row, returning io_eof at the end, and
     * scan_column to copy one column of it straight into what the program
     * gave sql_rows_scan, which is dest. The driver can do that with
     * sql_convert_assign, passing scan_ctx along. next is then not called. */
    Error (*next_row)(void *self);
    Error (*scan_column)(void *self, DriverScanContext scan_ctx, Int index, Any dest);

    /* RowsNextResultSet. Set both or neither. has_next_result_set is asked at
     * the end of a result set. next_result_set moves to the next one, skipping
     * whatever rows are left in this one, and returns io_eof when there is no
     * next one. */
    bool (*has_next_result_set)(void *self);
    Error (*next_result_set)(void *self);

    /* RowsColumnTypeScanType: the type a column scans into without losing
     * anything, TYPE_INT64 for a bigint for instance. */
    const Type *(*column_type_scan_type)(void *self, Int index);
    /* RowsColumnTypeDatabaseTypeName: the database's name for a column's type,
     * in capitals and without a length: "VARCHAR", "BIGINT", "JSONB". Belongs
     * to the Rows. */
    Str (*column_type_database_type_name)(void *self, Int index);
    /* RowsColumnTypeLength: the length of a variable length column, such as a
     * TEXT or a BLOB, with *ok false for a type that has none. A length with
     * no limit is INT64_MAX. */
    int64_t (*column_type_length)(void *self, Int index, bool *ok);
    /* RowsColumnTypeNullable: whether a column may be NULL, with *ok false
     * when the driver does not know. */
    bool (*column_type_nullable)(void *self, Int index, bool *ok);
    /* RowsColumnTypePrecisionScale: a decimal column's precision, with its
     * scale in *scale, and *ok false for a column that is not a decimal. */
    int64_t (*column_type_precision_scale)(void *self, Int index, int64_t *scale,
                                           bool *ok);
} DriverRowsVT;

typedef struct DriverRows {
    const DriverRowsVT *vt;
    void *data;
} DriverRows;

/* The optional interfaces of a Rows, by their Go names. */
typedef DriverRowsVT DriverRowsColumnScannerVT;
typedef DriverRows DriverRowsColumnScanner;
typedef DriverRowsVT DriverRowsNextResultSetVT;
typedef DriverRows DriverRowsNextResultSet;
typedef DriverRowsVT DriverRowsColumnTypeScanTypeVT;
typedef DriverRows DriverRowsColumnTypeScanType;
typedef DriverRowsVT DriverRowsColumnTypeDatabaseTypeNameVT;
typedef DriverRows DriverRowsColumnTypeDatabaseTypeName;
typedef DriverRowsVT DriverRowsColumnTypeLengthVT;
typedef DriverRows DriverRowsColumnTypeLength;
typedef DriverRowsVT DriverRowsColumnTypeNullableVT;
typedef DriverRows DriverRowsColumnTypeNullable;
typedef DriverRowsVT DriverRowsColumnTypePrecisionScaleVT;
typedef DriverRows DriverRowsColumnTypePrecisionScale;

/* -------------------------------------------------------------------- Stmt */

/* driver.Stmt, a prepared statement. It is bound to the Conn that made it and
 * is never used by two goroutines at once. close is called once, and may come
 * while a Rows from the statement is still open, which a driver that cannot
 * cope with has to wait out. */
typedef struct DriverStmtVT {
    const Type *self_type;
    Error (*close)(void *self);
    /* How many placeholders the statement has, or -1 when the driver does not
     * know. When it is 0 or more, database/sql checks the argument count
     * before calling exec or query. */
    Int (*num_input)(void *self);
    /* args is a Slice of DriverValue. Superseded by exec_context and
     * query_context, which a driver should set. */
    DriverResult (*exec)(void *self, Alloc *a, Slice args, Error *err);
    DriverRows (*query)(void *self, Slice args, Error *err);

    /* StmtExecContext and StmtQueryContext. args is a Slice of
     * DriverNamedValue. ctx is for the call only, and a driver must not keep
     * it. */
    DriverResult (*exec_context)(void *self, Alloc *a, Context ctx, Slice args,
                                 Error *err);
    DriverRows (*query_context)(void *self, Context ctx, Slice args, Error *err);

    /* NamedValueChecker, the same as the Conn's below, and asked first. */
    Error (*check_named_value)(void *self, Alloc *a, DriverNamedValue *nv);

    /* ColumnConverter, deprecated in Go in favour of check_named_value: the
     * converter for the argument at idx. */
    DriverValueConverter (*column_converter)(void *self, Int idx);
} DriverStmtVT;

typedef struct DriverStmt {
    const DriverStmtVT *vt;
    void *data;
} DriverStmt;

typedef DriverStmtVT DriverStmtExecContextVT;
typedef DriverStmt DriverStmtExecContext;
typedef DriverStmtVT DriverStmtQueryContextVT;
typedef DriverStmt DriverStmtQueryContext;
typedef DriverStmtVT DriverColumnConverterVT;
typedef DriverStmt DriverColumnConverter;

/* -------------------------------------------------------------------- Conn */

/* driver.Conn, a connection to the database. database/sql never uses one from
 * two goroutines at once. close is called once, and the driver frees it there. */
typedef struct DriverConnVT {
    const Type *self_type;
    /* A prepared statement for query. */
    DriverStmt (*prepare)(void *self, Str query, Error *err);
    /* Ends the connection. Go lets a driver close a Conn while its statements
     * and transactions are still open, and database/sql closes them first
     * anyway. */
    Error (*close)(void *self);
    /* Superseded by begin_tx, which a driver should set. */
    DriverTx (*begin)(void *self, Error *err);

    /* ConnPrepareContext, prepare with a context. */
    DriverStmt (*prepare_context)(void *self, Context ctx, Str query, Error *err);
    /* ConnBeginTx, begin with a context and options. A level or a read only
     * transaction that the driver cannot do is an error, and so is a context
     * that ends before the transaction starts. The context is for the start
     * only, and database/sql rolls the transaction back when it ends. */
    DriverTx (*begin_tx)(void *self, Context ctx, DriverTxOptions opts, Error *err);
    /* Pinger, which checks that the connection still works. Return
     * driver_err_bad_conn when it does not. Without it sql_db_ping only
     * checks that a connection can be had. */
    Error (*ping)(void *self, Context ctx);

    /* Execer and ExecerContext, which run a statement without preparing it.
     * args is a Slice of DriverValue for exec and of DriverNamedValue for
     * exec_context. Without them, or when they return driver_err_skip,
     * database/sql prepares the statement, runs it, and closes it. */
    DriverResult (*exec)(void *self, Alloc *a, Str query, Slice args, Error *err);
    DriverResult (*exec_context)(void *self, Alloc *a, Context ctx, Str query,
                                 Slice args, Error *err);
    /* Queryer and QueryerContext, the same for a query. */
    DriverRows (*query)(void *self, Str query, Slice args, Error *err);
    DriverRows (*query_context)(void *self, Context ctx, Str query, Slice args,
                                Error *err);

    /* SessionResetter, called before a connection that has been used is used
     * again. driver_err_bad_conn means it cannot be, and it is closed. */
    Error (*reset_session)(void *self, Context ctx);
    /* Validator, asked before a connection goes back to the pool. false means
     * it should be closed instead. */
    bool (*is_valid)(void *self);
    /* NamedValueChecker. Checks, and may convert, an argument before it is
     * passed to the driver: return BURROW_NO_ERROR to accept nv as it now is,
     * driver_err_remove_argument to leave it out, driver_err_skip to have
     * database/sql convert it the usual way, or an error to refuse it. A
     * converted value comes from a, which lasts until the call the argument
     * is for has returned. */
    Error (*check_named_value)(void *self, Alloc *a, DriverNamedValue *nv);
} DriverConnVT;

typedef struct DriverConn {
    const DriverConnVT *vt;
    void *data;
} DriverConn;

/* The optional interfaces of a Conn, by their Go names. */
typedef DriverConnVT DriverConnPrepareContextVT;
typedef DriverConn DriverConnPrepareContext;
typedef DriverConnVT DriverConnBeginTxVT;
typedef DriverConn DriverConnBeginTx;
typedef DriverConnVT DriverPingerVT;
typedef DriverConn DriverPinger;
typedef DriverConnVT DriverExecerVT;
typedef DriverConn DriverExecer;
typedef DriverConnVT DriverExecerContextVT;
typedef DriverConn DriverExecerContext;
typedef DriverConnVT DriverQueryerVT;
typedef DriverConn DriverQueryer;
typedef DriverConnVT DriverQueryerContextVT;
typedef DriverConn DriverQueryerContext;
typedef DriverConnVT DriverSessionResetterVT;
typedef DriverConn DriverSessionResetter;
typedef DriverConnVT DriverValidatorVT;
typedef DriverConn DriverValidator;
typedef DriverConnVT DriverNamedValueCheckerVT;
typedef DriverConn DriverNamedValueChecker;

/* ---------------------------------------------------------- Driver, Connector */

struct DriverDriver;

/* driver.Connector, a driver with its configuration already worked out, for
 * sql_open_db. connect may be called from several goroutines at once. ctx is
 * for the dial only, and a driver must not keep it. */
typedef struct DriverConnectorVT {
    const Type *self_type;
    DriverConn (*connect)(void *self, Context ctx, Error *err);
    /* The driver the connector belongs to, for sql_db_driver. */
    struct DriverDriver (*driver)(void *self);
    /* io.Closer, which sql_db_close calls when it is set. */
    Error (*close)(void *self);
} DriverConnectorVT;

typedef struct DriverConnector {
    const DriverConnectorVT *vt;
    void *data;
} DriverConnector;

/* driver.Driver, which opens connections from a name in the driver's own
 * format. A driver registers itself with sql_register under a name, and
 * open may be called from several goroutines at once. */
typedef struct DriverDriverVT {
    const Type *self_type;
    DriverConn (*open)(void *self, Str name, Error *err);
    /* DriverContext, which parses the name once instead of on every open.
     * database/sql uses the Connector it returns for every connection. */
    DriverConnector (*open_connector)(void *self, Str name, Error *err);
} DriverDriverVT;

typedef struct DriverDriver {
    const DriverDriverVT *vt;
    void *data;
} DriverDriver;

typedef DriverDriverVT DriverDriverContextVT;
typedef DriverDriver DriverDriverContext;

#ifdef __cplusplus
}
#endif

#endif /* BURROW_DATABASE_SQL_DRIVER_H */
