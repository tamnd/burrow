/* Derived from Go's src/database/sql/convert.go and the small types in
 * src/database/sql/sql.go: the driver registry, NamedArg, IsolationLevel and
 * the nullable types.
 * Go source: go1.27.1.
 *
 * Go's convertAssignRows does its general case with reflect. Here the same
 * cases go by the destination's descriptor: its kind, and the pointer, string
 * and byte slice types it names. What Go stores without copying, because its
 * collector keeps the source alive, is copied into the scan's allocator,
 * except RawBytes, which point into the Rows as they do in Go.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/database/sql.h"

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/database/sql/driver.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/iface.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/sync.h"
#include "burrow/time.h"
#include "burrow/type.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"
#include "burrow/uuid.h"

#include "driver_internal.h"
#include "sql_internal.h"

#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------- the registry */

typedef struct SqlDriverEntry {
    Str name;
    Driver driver;
} SqlDriverEntry;

static SyncRWMutex sql_drivers_mu;
static SqlDriverEntry *sql_drivers_list;
static Int sql_drivers_len;
static Int sql_drivers_cap;

void sql_register(Str name, Driver driver) {
    sync_rw_mutex_lock(&sql_drivers_mu);
    if (driver.vt == NULL) {
        sync_rw_mutex_unlock(&sql_drivers_mu);
        panic_str(BURROW_S("sql: Register driver is nil"));
    }
    for (Int i = 0; i < sql_drivers_len; i++) {
        if (str_eq(sql_drivers_list[i].name, name)) {
            sync_rw_mutex_unlock(&sql_drivers_mu);
            Str msg = fmt_sprintf_v(heap_allocator(),
                                    "sql: Register called twice for driver %s", name);
            panic_str(msg);
        }
    }
    if (sql_drivers_len == sql_drivers_cap) {
        Int ncap = sql_drivers_cap == 0 ? 8 : sql_drivers_cap * 2;
        SqlDriverEntry *n = mem_realloc(
            heap_allocator(), sql_drivers_list,
            (size_t)sql_drivers_cap * sizeof(SqlDriverEntry),
            (size_t)ncap * sizeof(SqlDriverEntry), _Alignof(SqlDriverEntry));
        if (n == NULL) {
            sync_rw_mutex_unlock(&sql_drivers_mu);
            panic_str(BURROW_S("sql: Register: out of memory"));
        }
        sql_drivers_list = n;
        sql_drivers_cap = ncap;
    }
    Str kept = str_clone(heap_allocator(), name);
    if (kept.len != name.len) {
        sync_rw_mutex_unlock(&sql_drivers_mu);
        panic_str(BURROW_S("sql: Register: out of memory"));
    }
    sql_drivers_list[sql_drivers_len].name = kept;
    sql_drivers_list[sql_drivers_len].driver = driver;
    sql_drivers_len++;
    sync_rw_mutex_unlock(&sql_drivers_mu);
}

Driver burrow__sql_driver_lookup(Str name, bool *ok) {
    Driver d = {NULL, NULL};
    *ok = false;
    sync_rw_mutex_r_lock(&sql_drivers_mu);
    for (Int i = 0; i < sql_drivers_len; i++) {
        if (str_eq(sql_drivers_list[i].name, name)) {
            d = sql_drivers_list[i].driver;
            *ok = true;
            break;
        }
    }
    sync_rw_mutex_r_unlock(&sql_drivers_mu);
    return d;
}

Slice sql_drivers(Alloc *a) {
    sync_rw_mutex_r_lock(&sql_drivers_mu);
    Slice out = {NULL, 0, 0, TYPE_STRING};
    Str *names = NULL;
    if (sql_drivers_len > 0)
        names = mem_alloc_array(a, (size_t)sql_drivers_len, sizeof(Str), _Alignof(Str));
    if (names != NULL) {
        Int n = 0;
        for (Int i = 0; i < sql_drivers_len; i++) {
            Str s = str_clone(a, sql_drivers_list[i].name);
            /* Insertion sort: there are a handful of drivers at most. */
            Int j = n;
            while (j > 0 && str_cmp(names[j - 1], s) > 0) {
                names[j] = names[j - 1];
                j--;
            }
            names[j] = s;
            n++;
        }
        out = (Slice){names, n, n, TYPE_STRING};
    }
    sync_rw_mutex_r_unlock(&sql_drivers_mu);
    return out;
}

/* ---------------------------------------------------------- descriptors */

#define SQL_FIELD(gonm, T, ctype, member)                                              \
    {{(const Byte *)(gonm), (Int)(sizeof(gonm) - 1)},                                  \
     {NULL, 0},                                                                        \
     (T),                                                                              \
     (uint32_t)offsetof(ctype, member)}

static const Field sql_named_arg_fields[] = {
    SQL_FIELD("Name", TYPE_STRING, SqlNamedArg, name),
    SQL_FIELD("Value", &burrow_type_Any, SqlNamedArg, value),
};

const Type burrow_type_SqlNamedArg = {
    {(const Byte *)"NamedArg", 8},
    {(const Byte *)"database/sql", 12},
    KIND_STRUCT,
    (uint32_t)sizeof(SqlNamedArg),
    (uint16_t)_Alignof(SqlNamedArg),
    2,
    0,
    sql_named_arg_fields,
    NULL,
    NULL,
    NULL,
    0,
    0x73716e61U, /* "sqna" */
    NULL,
};

static const Field sql_out_fields[] = {
    SQL_FIELD("Dest", &burrow_type_Any, SqlOut, dest),
    SQL_FIELD("In", TYPE_BOOL, SqlOut, in),
};

const Type burrow_type_SqlOut = {
    {(const Byte *)"Out", 3},
    {(const Byte *)"database/sql", 12},
    KIND_STRUCT,
    (uint32_t)sizeof(SqlOut),
    (uint16_t)_Alignof(SqlOut),
    2,
    0,
    sql_out_fields,
    NULL,
    NULL,
    NULL,
    0,
    0x73716f75U, /* "sqou" */
    NULL,
};

const Type burrow_type_SqlScanner = {
    {(const Byte *)"Scanner", 7},
    {(const Byte *)"database/sql", 12},
    KIND_INTERFACE,
    (uint32_t)sizeof(SqlScanner),
    (uint16_t)_Alignof(SqlScanner),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

const Type burrow_type_SqlRawBytes = {
    {(const Byte *)"RawBytes", 8},
    {(const Byte *)"database/sql", 12},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    TYPE_BYTE,
    NULL,
    0,
    0x73717262U, /* "sqrb" */
    NULL,
};

SqlNamedArg sql_named(Str name, Any value) {
    return (SqlNamedArg){name, value};
}

Str sql_isolation_level_string(SqlIsolationLevel i, Alloc *a) {
    switch (i) {
    case SQL_LEVEL_DEFAULT:
        return BURROW_S("Default");
    case SQL_LEVEL_READ_UNCOMMITTED:
        return BURROW_S("Read Uncommitted");
    case SQL_LEVEL_READ_COMMITTED:
        return BURROW_S("Read Committed");
    case SQL_LEVEL_WRITE_COMMITTED:
        return BURROW_S("Write Committed");
    case SQL_LEVEL_REPEATABLE_READ:
        return BURROW_S("Repeatable Read");
    case SQL_LEVEL_SNAPSHOT:
        return BURROW_S("Snapshot");
    case SQL_LEVEL_SERIALIZABLE:
        return BURROW_S("Serializable");
    case SQL_LEVEL_LINEARIZABLE:
        return BURROW_S("Linearizable");
    default:
        return fmt_sprintf_v(a, "IsolationLevel(%d)", i);
    }
}

/* ------------------------------------------------------------- helpers */

BURROW_SENTINEL_ERROR(sql_err_nil_ptr, "destination pointer is nil");

/* []byte, as opposed to RawBytes or a named byte slice type. */
static bool sc_is_bytes(const Type *t) {
    return t != NULL && t->kind == KIND_SLICE && t->name.len == 0 && t->elem != NULL &&
           t->elem->kind == KIND_UINT8;
}

static bool sc_is_int_kind(Kind k) {
    return k == KIND_INT || k == KIND_INT8 || k == KIND_INT16 || k == KIND_INT32 ||
           k == KIND_INT64;
}

/* Go's lists of unsigned kinds leave out uintptr. */
static bool sc_is_uint_kind(Kind k) {
    return k == KIND_UINT || k == KIND_UINT8 || k == KIND_UINT16 || k == KIND_UINT32 ||
           k == KIND_UINT64;
}

static int64_t sc_int_of(const Type *t, const void *p) {
    switch (t->size) {
    case 1:
        return *(const int8_t *)p;
    case 2:
        return *(const int16_t *)p;
    case 4:
        return *(const int32_t *)p;
    default:
        return *(const int64_t *)p;
    }
}

static uint64_t sc_uint_of(const Type *t, const void *p) {
    switch (t->size) {
    case 1:
        return *(const uint8_t *)p;
    case 2:
        return *(const uint16_t *)p;
    case 4:
        return *(const uint32_t *)p;
    default:
        return *(const uint64_t *)p;
    }
}

static void sc_set_int(const Type *t, void *p, int64_t v) {
    switch (t->size) {
    case 1:
        *(int8_t *)p = (int8_t)v;
        break;
    case 2:
        *(int16_t *)p = (int16_t)v;
        break;
    case 4:
        *(int32_t *)p = (int32_t)v;
        break;
    default:
        *(int64_t *)p = v;
        break;
    }
}

static void sc_set_uint(const Type *t, void *p, uint64_t v) {
    switch (t->size) {
    case 1:
        *(uint8_t *)p = (uint8_t)v;
        break;
    case 2:
        *(uint16_t *)p = (uint16_t)v;
        break;
    case 4:
        *(uint32_t *)p = (uint32_t)v;
        break;
    default:
        *(uint64_t *)p = v;
        break;
    }
}

/* reflect.Type.Bits. */
static Int sc_bits(const Type *t) {
    return (Int)t->size * 8;
}

static double sc_float_of(const Type *t, const void *p) {
    return t->size == sizeof(float) ? (double)*(const float *)p : *(const double *)p;
}

static Str sc_bytes_str(Bytes b) {
    return str_from_bytes(b.p, b.len);
}

static Bytes sc_str_bytes(Str s) {
    return (Bytes){(void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE};
}

/* bytes.Clone, which keeps nil as nil and empty as empty. */
static Bytes sc_clone_bytes(Alloc *a, Bytes b) {
    if (b.p == NULL)
        return (Bytes){NULL, 0, 0, TYPE_BYTE};
    Bytes out = slice_append(a, (Bytes){NULL, 0, 0, TYPE_BYTE}, b.p, b.len);
    if (out.p == NULL) {
        /* An empty clone is still not nil. */
        static const Byte empty[1] = {0};
        out = (Bytes){(void *)(uintptr_t)empty, 0, 0, TYPE_BYTE};
    }
    return out;
}

static Bytes sc_append(Alloc *a, Bytes buf, Str s) {
    buf.elem = TYPE_BYTE;
    return slice_append(a, buf, s.p, s.len);
}

/* asString. */
static Str sc_as_string(Alloc *a, Any src) {
    const Type *t = src.t;
    if (t == TYPE_STRING)
        return *(const Str *)src.data;
    if (sc_is_bytes(t))
        return sc_bytes_str(*(const Bytes *)src.data);
    if (t != NULL && src.data != NULL) {
        Kind k = t->kind;
        if (sc_is_int_kind(k))
            return strconv_format_int(a, sc_int_of(t, src.data), 10);
        if (sc_is_uint_kind(k))
            return strconv_format_uint(a, sc_uint_of(t, src.data), 10);
        if (k == KIND_FLOAT64)
            return strconv_format_float(a, *(const double *)src.data, 'g', -1, 64);
        if (k == KIND_FLOAT32)
            return strconv_format_float(a, (double)*(const float *)src.data, 'g', -1,
                                        32);
        if (k == KIND_BOOL)
            return strconv_format_bool(*(const bool *)src.data);
    }
    return fmt_sprintf_v(a, "%v", src);
}

/* asBytes. */
static bool sc_as_bytes(Alloc *a, Bytes buf, Any src, Bytes *out) {
    const Type *t = src.t;
    if (t == NULL || src.data == NULL)
        return false;
    buf.elem = TYPE_BYTE;
    Kind k = t->kind;
    if (sc_is_int_kind(k)) {
        *out = strconv_append_int(a, buf, sc_int_of(t, src.data), 10);
        return true;
    }
    if (sc_is_uint_kind(k)) {
        *out = strconv_append_uint(a, buf, sc_uint_of(t, src.data), 10);
        return true;
    }
    if (k == KIND_FLOAT32 || k == KIND_FLOAT64) {
        Str s = strconv_format_float(a, sc_float_of(t, src.data), 'g', -1,
                                     k == KIND_FLOAT32 ? 32 : 64);
        *out = sc_append(a, buf, s);
        return true;
    }
    if (k == KIND_BOOL) {
        *out = strconv_append_bool(a, buf, *(const bool *)src.data);
        return true;
    }
    if (k == KIND_STRING) {
        *out = sc_append(a, buf, *(const Str *)src.data);
        return true;
    }
    return false;
}

/* strconvErr. */
static Error sc_strconv_err(Error err) {
    const StrconvNumError *ne = errors_as(err, TYPE_STRCONV_NUM_ERROR);
    return ne != NULL ? ne->err : err;
}

/* A copy of v in a that outlives the driver's memory, for a destination of
 * type any. */
static Any sc_clone_any(Alloc *a, Any v, Error *err) {
    if (v.t == NULL || v.data == NULL)
        return (Any){NULL, NULL};
    void *p =
        mem_alloc(a, v.t->size > 0 ? v.t->size : 1, v.t->align > 0 ? v.t->align : 1);
    if (p == NULL) {
        *err = burrow_err_out_of_memory;
        return (Any){NULL, NULL};
    }
    if (v.t->kind == KIND_STRING) {
        *(Str *)p = str_clone(a, *(const Str *)v.data);
    } else if (sc_is_bytes(v.t)) {
        *(Bytes *)p = sc_clone_bytes(a, *(const Bytes *)v.data);
    } else {
        memcpy(p, v.data, v.t->size);
    }
    return (Any){v.t, p};
}

/* The text of a destination's type for an error, which Go prints as the
 * pointer it was given. */
static Error sc_unsupported(Any src, Any dest) {
    Any dt = {dest.t, NULL};
    /* A SqlRowsPtr destination is Go's *Rows, the pointer itself. */
    if (dest.t == &burrow_type_SqlRowsPtr)
        return fmt_errorf_v(
            "unsupported Scan, storing driver.Value type %T into type %T", src, dt);
    return fmt_errorf_v("unsupported Scan, storing driver.Value type %T into type *%T",
                        src, dt);
}

static bool sc_scan_shape(const Method *m) {
    if (m == NULL || m->ftype == NULL || m->thunk == NULL)
        return false;
    const Type *f = m->ftype;
    return type_num_in(f) == 2 && type_num_out(f) == 1 &&
           type_in(f, 0) == &burrow_type_DriverAllocArg && type_in(f, 1) == TYPE_ANY &&
           type_out(f, 0) == TYPE_ERROR;
}

static bool sc_compose_shape(const Method *m) {
    if (m == NULL || m->ftype == NULL || m->thunk == NULL)
        return false;
    const Type *f = m->ftype;
    return type_num_in(f) == 1 && type_num_out(f) == 1 &&
           type_in(f, 0) == &burrow_type_DriverDecomposed &&
           type_out(f, 0) == TYPE_ERROR;
}

/* ------------------------------------------------------- convertAssignRows */

static Error sc_assign_string(Alloc *a, Any dest, Str s, SqlRows *rows) {
    const Type *dt = dest.t;
    if (dt == TYPE_STRING) {
        Str c = str_clone(a, s);
        if (c.len != s.len)
            return burrow_err_out_of_memory;
        *(Str *)dest.data = c;
        return BURROW_NO_ERROR;
    }
    if (sc_is_bytes(dt)) {
        *(Bytes *)dest.data = sc_clone_bytes(a, sc_str_bytes(s));
        return BURROW_NO_ERROR;
    }
    if (dt == &burrow_type_SqlRawBytes) {
        Alloc *ra = burrow__sql_rows_raw_alloc(rows, a);
        *(Bytes *)dest.data = burrow__sql_rows_setrawbuf(
            rows, sc_append(ra, burrow__sql_rows_rawbuf(rows), s));
        return BURROW_NO_ERROR;
    }
    if (dt == TYPE_UUID) {
        Error e = BURROW_NO_ERROR;
        Uuid u = uuid_parse_str(s, &e);
        if (BURROW_FAILED(e))
            return fmt_errorf_v(
                "converting driver.Value type string (%q) to a UUID: %v", s, e);
        *(Uuid *)dest.data = u;
        return BURROW_NO_ERROR;
    }
    return (Error){NULL, NULL};
}

static Error sc_assign_bytes(Alloc *a, Any dest, Bytes s) {
    const Type *dt = dest.t;
    if (dt == TYPE_STRING) {
        Str c = str_clone(a, sc_bytes_str(s));
        if (c.len != s.len)
            return burrow_err_out_of_memory;
        *(Str *)dest.data = c;
        return BURROW_NO_ERROR;
    }
    if (dt == TYPE_ANY) {
        Bytes *b = BURROW_NEW(a, Bytes);
        if (b == NULL)
            return burrow_err_out_of_memory;
        *b = sc_clone_bytes(a, s);
        *(Any *)dest.data = (Any){TYPE_BYTES, b};
        return BURROW_NO_ERROR;
    }
    if (sc_is_bytes(dt)) {
        *(Bytes *)dest.data = sc_clone_bytes(a, s);
        return BURROW_NO_ERROR;
    }
    if (dt == &burrow_type_SqlRawBytes) {
        s.elem = TYPE_BYTE;
        *(Bytes *)dest.data = s;
        return BURROW_NO_ERROR;
    }
    if (dt == TYPE_UUID) {
        Uuid *d = dest.data;
        if (s.len == (Int)sizeof d->b) {
            memcpy(d->b, s.p, sizeof d->b);
            return BURROW_NO_ERROR;
        }
        Uuid u;
        memset(&u, 0, sizeof u);
        Error e = uuid_unmarshal_text(&u, s);
        if (BURROW_FAILED(e))
            return fmt_errorf_v(
                "converting driver.Value type []byte (%q) to a UUID: %v",
                sc_bytes_str(s), e);
        *d = u;
        return BURROW_NO_ERROR;
    }
    return (Error){NULL, NULL};
}

static Error sc_assign_time(Alloc *a, Any dest, Time s, SqlRows *rows) {
    const Type *dt = dest.t;
    if (dt == TYPE_TIME) {
        *(Time *)dest.data = s;
        return BURROW_NO_ERROR;
    }
    if (dt == TYPE_STRING) {
        *(Str *)dest.data = time_format(s, a, TIME_RFC3339_NANO);
        return BURROW_NO_ERROR;
    }
    if (sc_is_bytes(dt)) {
        Bytes buf = {NULL, 0, 0, TYPE_BYTE};
        *(Bytes *)dest.data = time_append_format(s, a, buf, TIME_RFC3339_NANO);
        return BURROW_NO_ERROR;
    }
    if (dt == &burrow_type_SqlRawBytes) {
        Alloc *ra = burrow__sql_rows_raw_alloc(rows, a);
        *(Bytes *)dest.data = burrow__sql_rows_setrawbuf(
            rows, time_append_format(s, ra, burrow__sql_rows_rawbuf(rows),
                                     TIME_RFC3339_NANO));
        return BURROW_NO_ERROR;
    }
    return (Error){NULL, NULL};
}

/* Whether e says "not handled here", the zero Error the helpers above return
 * when the destination is not one of theirs. */
static bool sc_unhandled(Error e) {
    return e.vt == NULL && e.data == NULL;
}

Error burrow__sql_convert_assign_rows(Alloc *a, Any dest, Any src, SqlRows *rows) {
    src = burrow__driver_unwrap(src);
    const Type *dt = dest.t;
    const Type *st = src.t;
    if (dt == NULL)
        return fmt_errorf_v("destination not a pointer");
    if (dest.data == NULL)
        return sql_err_nil_ptr;
    if (st != NULL && src.data == NULL)
        st = NULL;

    /* Common cases, without reflect. */
    if (st == TYPE_STRING) {
        Error e = sc_assign_string(a, dest, *(const Str *)src.data, rows);
        if (!sc_unhandled(e))
            return e;
    } else if (sc_is_bytes(st)) {
        Error e = sc_assign_bytes(a, dest, *(const Bytes *)src.data);
        if (!sc_unhandled(e))
            return e;
    } else if (st == TYPE_TIME) {
        Error e = sc_assign_time(a, dest, *(const Time *)src.data, rows);
        if (!sc_unhandled(e))
            return e;
    } else if (st == NULL) {
        if (dt == TYPE_ANY) {
            *(Any *)dest.data = (Any){NULL, NULL};
            return BURROW_NO_ERROR;
        }
        if (sc_is_bytes(dt) || dt == &burrow_type_SqlRawBytes) {
            *(Bytes *)dest.data = (Bytes){NULL, 0, 0, TYPE_BYTE};
            return BURROW_NO_ERROR;
        }
    } else if (st == &burrow_type_DriverRows) {
        /* The driver is returning a cursor the client may iterate over. */
        if (dt == &burrow_type_SqlRowsPtr) {
            if (rows == NULL)
                return fmt_errorf_v(
                    "invalid context to convert cursor rows, missing parent *Rows");
            Error e = BURROW_NO_ERROR;
            SqlRows *child =
                burrow__sql_rows_cursor(rows, *(const DriverRows *)src.data, &e);
            if (BURROW_FAILED(e))
                return e;
            *(SqlRows **)dest.data = child;
            return BURROW_NO_ERROR;
        }
    } else if (dt != TYPE_ANY && burrow__driver_is_decimal(src)) {
        burrow__DriverFound f;
        if (burrow__driver_find(dest, BURROW_S("Compose"), NULL, sc_compose_shape,
                                &f)) {
            DriverDecomposed parts =
                burrow__driver_decompose(src, (Bytes){NULL, 0, 0, TYPE_BYTE});
            Error e = BURROW_NO_ERROR;
            void *args[1] = {(void *)&parts};
            void *rets[1] = {&e};
            method_call(f.m, f.recv, args, rets);
            return e;
        }
    }

    if (dt == TYPE_STRING) {
        Kind k = st != NULL ? st->kind : KIND_INVALID;
        if (k == KIND_BOOL || sc_is_int_kind(k) || sc_is_uint_kind(k) ||
            k == KIND_FLOAT32 || k == KIND_FLOAT64) {
            *(Str *)dest.data = sc_as_string(a, src);
            return BURROW_NO_ERROR;
        }
    } else if (sc_is_bytes(dt)) {
        Bytes b;
        if (sc_as_bytes(a, (Bytes){NULL, 0, 0, TYPE_BYTE}, src, &b)) {
            *(Bytes *)dest.data = b;
            return BURROW_NO_ERROR;
        }
    } else if (dt == &burrow_type_SqlRawBytes) {
        Bytes b;
        Alloc *ra = burrow__sql_rows_raw_alloc(rows, a);
        if (sc_as_bytes(ra, burrow__sql_rows_rawbuf(rows), src, &b)) {
            *(Bytes *)dest.data = burrow__sql_rows_setrawbuf(rows, b);
            return BURROW_NO_ERROR;
        }
    } else if (dt == TYPE_BOOL) {
        Error e = BURROW_NO_ERROR;
        DriverValue bv = driver_bool.vt->convert_value(driver_bool.data, a, src, &e);
        if (!BURROW_FAILED(e) && bv.data != NULL)
            *(bool *)dest.data = *(const bool *)bv.data;
        return e;
    } else if (dt == TYPE_ANY) {
        Error e = BURROW_NO_ERROR;
        Any c = sc_clone_any(a, src, &e);
        if (BURROW_FAILED(e))
            return e;
        *(Any *)dest.data = c;
        return BURROW_NO_ERROR;
    }

    if (dt->kind != KIND_POINTER) {
        burrow__DriverFound f;
        if (burrow__driver_find(dest, BURROW_S("Scan"), &burrow_type_SqlScanner,
                                sc_scan_shape, &f)) {
            if (f.vt != NULL)
                return ((const SqlScannerVT *)f.vt)->scan(f.recv, a, src);
            Error e = BURROW_NO_ERROR;
            DriverAllocArg aa = a;
            void *args[2] = {(void *)&aa, (void *)&src};
            void *rets[1] = {&e};
            method_call(f.m, f.recv, args, rets);
            return e;
        }
    }

    void *dv = dest.data;
    Kind dk = dt->kind;
    if (st != NULL) {
        /* AssignableTo and ConvertibleTo between types of the same kind. */
        bool same = type_same(st, dt);
        if (!same && st->kind == dk && dt->size == st->size &&
            (dk == KIND_BOOL || sc_is_int_kind(dk) || sc_is_uint_kind(dk) ||
             dk == KIND_FLOAT32 || dk == KIND_FLOAT64 || dk == KIND_STRING))
            same = true;
        if (!same && sc_is_bytes(st) && dk == KIND_SLICE && dt->elem != NULL &&
            dt->elem->kind == KIND_UINT8)
            same = true;
        if (same) {
            if (st->kind == KIND_STRING) {
                Str s = *(const Str *)src.data;
                Str c = str_clone(a, s);
                if (c.len != s.len)
                    return burrow_err_out_of_memory;
                *(Str *)dv = c;
            } else if (st->kind == KIND_SLICE && st->elem != NULL &&
                       st->elem->kind == KIND_UINT8) {
                Bytes b = sc_clone_bytes(a, *(const Bytes *)src.data);
                b.elem = dt->elem;
                *(Bytes *)dv = b;
            } else {
                memcpy(dv, src.data, dt->size);
            }
            return BURROW_NO_ERROR;
        }
    }

    /* The following conversions use a string value as an intermediate
     * representation to convert between various numeric types.
     *
     * This also allows scanning into user defined types such as
     * "type Int int64". For symmetry, also check for string destination types. */
    if (dk == KIND_POINTER) {
        if (st == NULL) {
            *(void **)dv = NULL;
            return BURROW_NO_ERROR;
        }
        const Type *et = dt->elem;
        if (et == NULL)
            return sc_unsupported(src, dest);
        void *np =
            mem_alloc(a, et->size > 0 ? et->size : 1, et->align > 0 ? et->align : 1);
        if (np == NULL)
            return burrow_err_out_of_memory;
        *(void **)dv = np;
        return burrow__sql_convert_assign_rows(a, (Any){et, np}, src, rows);
    }
    if (sc_is_int_kind(dk)) {
        if (st == NULL)
            return fmt_errorf_v("converting NULL to %s is unsupported", kind_name(dk));
        Str s = sc_as_string(a, src);
        Error e = BURROW_NO_ERROR;
        int64_t i64 = strconv_parse_int(s, 10, sc_bits(dt), &e);
        if (BURROW_FAILED(e))
            return fmt_errorf_v("converting driver.Value type %T (%q) to a %s: %v", src,
                                s, kind_name(dk), sc_strconv_err(e));
        sc_set_int(dt, dv, i64);
        return BURROW_NO_ERROR;
    }
    if (sc_is_uint_kind(dk)) {
        if (st == NULL)
            return fmt_errorf_v("converting NULL to %s is unsupported", kind_name(dk));
        Str s = sc_as_string(a, src);
        Error e = BURROW_NO_ERROR;
        uint64_t u64 = strconv_parse_uint(s, 10, sc_bits(dt), &e);
        if (BURROW_FAILED(e))
            return fmt_errorf_v("converting driver.Value type %T (%q) to a %s: %v", src,
                                s, kind_name(dk), sc_strconv_err(e));
        sc_set_uint(dt, dv, u64);
        return BURROW_NO_ERROR;
    }
    if (dk == KIND_FLOAT32 || dk == KIND_FLOAT64) {
        if (st == NULL)
            return fmt_errorf_v("converting NULL to %s is unsupported", kind_name(dk));
        Str s = sc_as_string(a, src);
        Error e = BURROW_NO_ERROR;
        double f64 = strconv_parse_float(s, sc_bits(dt), &e);
        if (BURROW_FAILED(e))
            return fmt_errorf_v("converting driver.Value type %T (%q) to a %s: %v", src,
                                s, kind_name(dk), sc_strconv_err(e));
        if (dk == KIND_FLOAT32)
            *(float *)dv = (float)f64;
        else
            *(double *)dv = f64;
        return BURROW_NO_ERROR;
    }
    if (dk == KIND_STRING) {
        if (st == NULL)
            return fmt_errorf_v("converting NULL to %s is unsupported", kind_name(dk));
        if (st == TYPE_STRING || sc_is_bytes(st)) {
            Str s = st == TYPE_STRING ? *(const Str *)src.data
                                      : sc_bytes_str(*(const Bytes *)src.data);
            Str c = str_clone(a, s);
            if (c.len != s.len)
                return burrow_err_out_of_memory;
            *(Str *)dv = c;
            return BURROW_NO_ERROR;
        }
    }

    return sc_unsupported(src, dest);
}

Error sql_convert_assign(Alloc *a, DriverScanContext scan_ctx, Any dest,
                         DriverValue src) {
    SqlRows *rows = NULL;
    if (scan_ctx.burrow__v.t == &burrow__sql_rows_type)
        rows = scan_ctx.burrow__v.data;
    return burrow__sql_convert_assign_rows(a, dest, src, rows);
}

/* ---------------------------------------------------- driverArgsConnLocked */

static Str sc_describe_named_value(const DriverNamedValue *nv) {
    if (nv->name.len == 0)
        return fmt_sprintf_v(heap_allocator(), "$%d", nv->ordinal);
    return fmt_sprintf_v(heap_allocator(), "with name %q", nv->name);
}

static Error sc_validate_named_value_name(Str name) {
    if (name.len == 0)
        return BURROW_NO_ERROR;
    Int size = 0;
    Rune r = utf8_decode_rune_in_string(name, &size);
    if (unicode_is_letter(r))
        return BURROW_NO_ERROR;
    return fmt_errorf_v("name %q does not begin with a letter", name);
}

/* ccChecker. */
typedef struct SqlCCChecker {
    const DriverStmt *cci;
    Int want;
} SqlCCChecker;

static Error sc_cc_check(Alloc *a, const SqlCCChecker *c, DriverNamedValue *nv) {
    if (c->cci == NULL)
        return driver_err_skip;
    /* The column converter shouldn't be called on any index it isn't
     * expecting. The final error will be thrown in the argument converter
     * loop. */
    Int index = nv->ordinal - 1;
    if (c->want >= 0 && c->want <= index)
        return BURROW_NO_ERROR;

    /* First, see if the value itself knows how to convert itself to a driver
     * type. For example, a NullString struct changing into a string or nil. */
    if (burrow__driver_is_valuer(nv->value)) {
        Error e = BURROW_NO_ERROR;
        DriverValue sv = burrow__driver_call_valuer(a, nv->value, &e);
        if (BURROW_FAILED(e))
            return e;
        if (!driver_is_value(sv))
            return fmt_errorf_v("non-subset type %T returned from Value", sv);
        nv->value = sv;
    }

    /* Second, ask the column to sanity check itself. */
    Any arg = nv->value;
    DriverValueConverter conv = c->cci->vt->column_converter(c->cci->data, index);
    Error e = BURROW_NO_ERROR;
    nv->value = conv.vt->convert_value(conv.data, a, arg, &e);
    if (BURROW_FAILED(e))
        return e;
    if (!driver_is_value(nv->value))
        return fmt_errorf_v(
            "driver ColumnConverter error converted %T to unsupported type %T", arg,
            nv->value);
    return BURROW_NO_ERROR;
}

static Error sc_default_check(Alloc *a, DriverNamedValue *nv) {
    Error e = BURROW_NO_ERROR;
    nv->value = driver_default_parameter_converter.vt->convert_value(
        driver_default_parameter_converter.data, a, nv->value, &e);
    return e;
}

typedef enum SqlChecker { SQL_CHECK_DEFAULT, SQL_CHECK_NVC, SQL_CHECK_CC } SqlChecker;

Slice burrow__sql_driver_args_conn_locked(Alloc *a, DriverConn ci, const DriverStmt *si,
                                          Slice args, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Int nargs = args.len;
    DriverNamedValue *nvargs = NULL;
    if (nargs > 0) {
        nvargs = mem_alloc_array(a, (size_t)nargs, sizeof(DriverNamedValue),
                                 _Alignof(DriverNamedValue));
        if (nvargs == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return (Slice){NULL, 0, 0, TYPE_DRIVER_NAMED_VALUE};
        }
    }
    Int nvlen = nargs;

    /* -1 means the driver doesn't know how to count the number of
     * placeholders, so we won't sanity check input here and instead let the
     * driver deal with errors. */
    Int want = -1;
    SqlCCChecker cc = {NULL, -1};
    if (si != NULL) {
        want = si->vt->num_input != NULL ? si->vt->num_input(si->data) : -1;
        cc.want = want;
    }

    /* Check all types of interfaces from the start. Drivers may opt to use the
     * NamedValueChecker for special argument types, then return
     * driver_err_skip to pass it along to the column converter. */
    Error (*nvc)(void *self, Alloc *a, DriverNamedValue *nv) = NULL;
    void *nvc_self = NULL;
    if (si != NULL && si->vt->check_named_value != NULL) {
        nvc = si->vt->check_named_value;
        nvc_self = si->data;
    } else if (ci.vt != NULL && ci.vt->check_named_value != NULL) {
        nvc = ci.vt->check_named_value;
        nvc_self = ci.data;
    }
    bool have_cci = si != NULL && si->vt->column_converter != NULL;
    if (have_cci)
        cc.cci = si;

    /* Loop through all the arguments, checking each one. If no error is
     * returned simply increment the index and continue. However, if
     * driver_err_remove_argument is returned the argument is not included in
     * the query argument list. */
    Int n = 0;
    const Any *in = args.p;
    for (Int i = 0; i < nargs; i++) {
        DriverNamedValue *nv = &nvargs[n];
        Any arg = burrow__driver_unwrap(in[i]);
        nv->name = BURROW_STR_EMPTY;
        if (arg.t == &burrow_type_SqlNamedArg && arg.data != NULL) {
            const SqlNamedArg *np = arg.data;
            Error e = sc_validate_named_value_name(np->name);
            if (BURROW_FAILED(e)) {
                BURROW_OUT(err, e);
                return (Slice){NULL, 0, 0, TYPE_DRIVER_NAMED_VALUE};
            }
            arg = np->value;
            nv->name = np->name;
        }
        nv->ordinal = n + 1;
        nv->value = arg;

        /* Checking sequence has four routes:
         * A: 1. Default
         * B: 1. NamedValueChecker 2. Column Converter 3. Default
         * C: 1. NamedValueChecker 3. Default
         * D: 1. Column Converter 2. Default
         *
         * The only time a Column Converter is called is first or after
         * NamedValueConverter. If first it is handled before the nextCheck
         * label. Thus for repeats tries only when the NamedValueConverter is
         * selected should the Column Converter be used in the retry. */
        SqlChecker checker = SQL_CHECK_DEFAULT;
        bool next_cc = false;
        if (nvc != NULL) {
            next_cc = have_cci;
            checker = SQL_CHECK_NVC;
        } else if (have_cci) {
            checker = SQL_CHECK_CC;
        }

        for (;;) {
            Error e = BURROW_NO_ERROR;
            if (checker == SQL_CHECK_NVC)
                e = nvc(nvc_self, a, nv);
            else if (checker == SQL_CHECK_CC)
                e = sc_cc_check(a, &cc, nv);
            else
                e = sc_default_check(a, nv);
            if (!BURROW_FAILED(e)) {
                n++;
                break;
            }
            if (errors_is(e, driver_err_remove_argument)) {
                nvlen--;
                break;
            }
            if (errors_is(e, driver_err_skip)) {
                if (next_cc) {
                    next_cc = false;
                    checker = SQL_CHECK_CC;
                } else {
                    checker = SQL_CHECK_DEFAULT;
                }
                continue;
            }
            BURROW_OUT(err, fmt_errorf_v("sql: converting argument %s type: %w",
                                         sc_describe_named_value(nv), e));
            return (Slice){NULL, 0, 0, TYPE_DRIVER_NAMED_VALUE};
        }
    }

    /* Check the length of arguments after conversion to allow for omitted
     * arguments. */
    if (want != -1 && nvlen != want) {
        BURROW_OUT(err,
                   fmt_errorf_v("sql: expected %d arguments, got %d", want, nvlen));
        return (Slice){NULL, 0, 0, TYPE_DRIVER_NAMED_VALUE};
    }
    return (Slice){nvargs, nvlen, nargs, TYPE_DRIVER_NAMED_VALUE};
}

/* ------------------------------------------------------- the nullable types */

#define SQL_NULL_SCAN(fn, T, member, ctype)                                            \
    Error fn(T *n, Alloc *a, Any value) {                                              \
        value = burrow__driver_unwrap(value);                                          \
        if (value.t == NULL || value.data == NULL) {                                   \
            memset(&n->member, 0, sizeof n->member);                                   \
            n->valid = false;                                                          \
            return BURROW_NO_ERROR;                                                    \
        }                                                                              \
        Error e = burrow__sql_convert_assign_rows(a, (Any){(ctype), &n->member},       \
                                                  value, NULL);                        \
        n->valid = !BURROW_FAILED(e);                                                  \
        return e;                                                                      \
    }

SQL_NULL_SCAN(sql_null_string_scan, SqlNullString, string, TYPE_STRING)
SQL_NULL_SCAN(sql_null_int64_scan, SqlNullInt64, int64, TYPE_INT64)
SQL_NULL_SCAN(sql_null_int32_scan, SqlNullInt32, int32, TYPE_INT32)
SQL_NULL_SCAN(sql_null_int16_scan, SqlNullInt16, int16, TYPE_INT16)
SQL_NULL_SCAN(sql_null_byte_scan, SqlNullByte, byte, TYPE_UINT8)
SQL_NULL_SCAN(sql_null_float64_scan, SqlNullFloat64, float64, TYPE_FLOAT64)
SQL_NULL_SCAN(sql_null_bool_scan, SqlNullBool, bool_, TYPE_BOOL)
SQL_NULL_SCAN(sql_null_time_scan, SqlNullTime, time, TYPE_TIME)

static DriverValue sc_box(Alloc *a, const Type *t, const void *v, size_t size,
                          Error *err) {
    void *p = mem_alloc(a, size, t->align);
    if (p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (DriverValue){NULL, NULL};
    }
    memcpy(p, v, size);
    return (DriverValue){t, p};
}

DriverValue sql_null_string_value(SqlNullString ns, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (!ns.valid)
        return (DriverValue){NULL, NULL};
    return sc_box(a, TYPE_STRING, &ns.string, sizeof ns.string, err);
}

DriverValue sql_null_int64_value(SqlNullInt64 n, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (!n.valid)
        return (DriverValue){NULL, NULL};
    return sc_box(a, TYPE_INT64, &n.int64, sizeof n.int64, err);
}

DriverValue sql_null_int32_value(SqlNullInt32 n, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (!n.valid)
        return (DriverValue){NULL, NULL};
    int64_t v = n.int32;
    return sc_box(a, TYPE_INT64, &v, sizeof v, err);
}

DriverValue sql_null_int16_value(SqlNullInt16 n, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (!n.valid)
        return (DriverValue){NULL, NULL};
    int64_t v = n.int16;
    return sc_box(a, TYPE_INT64, &v, sizeof v, err);
}

DriverValue sql_null_byte_value(SqlNullByte n, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (!n.valid)
        return (DriverValue){NULL, NULL};
    int64_t v = n.byte;
    return sc_box(a, TYPE_INT64, &v, sizeof v, err);
}

DriverValue sql_null_float64_value(SqlNullFloat64 n, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (!n.valid)
        return (DriverValue){NULL, NULL};
    return sc_box(a, TYPE_FLOAT64, &n.float64, sizeof n.float64, err);
}

DriverValue sql_null_bool_value(SqlNullBool n, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (!n.valid)
        return (DriverValue){NULL, NULL};
    return sc_box(a, TYPE_BOOL, &n.bool_, sizeof n.bool_, err);
}

DriverValue sql_null_time_value(SqlNullTime n, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (!n.valid)
        return (DriverValue){NULL, NULL};
    return sc_box(a, TYPE_TIME, &n.time, sizeof n.time, err);
}

Error sql_null_scan(SqlNull *n, Alloc *a, Any value) {
    value = burrow__driver_unwrap(value);
    if (value.t == NULL || value.data == NULL) {
        if (n->v.t != NULL && n->v.data != NULL)
            memset(n->v.data, 0, n->v.t->size);
        n->valid = false;
        return BURROW_NO_ERROR;
    }
    Error e = burrow__sql_convert_assign_rows(a, n->v, value, NULL);
    n->valid = !BURROW_FAILED(e);
    return e;
}

DriverValue sql_null_value(SqlNull n, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (!n.valid)
        return (DriverValue){NULL, NULL};
    Any v = n.v;
    /* See Go's issue 69728. */
    if (burrow__driver_is_valuer(v)) {
        Error e = BURROW_NO_ERROR;
        DriverValue val = burrow__driver_call_valuer(a, v, &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return val;
        }
        v = val;
    }
    /* See Go's issue 69837. */
    return driver_default_parameter_converter.vt->convert_value(
        driver_default_parameter_converter.data, a, v, err);
}

/* The Value methods take a value receiver, and a method thunk passes a
 * pointer, so each has a wrapper. */
#define SQL_NULL_VALUE_METHOD(mfn, T, fn)                                              \
    static Any mfn(T *n, DriverAllocArg a, DriverErrorArg err) {                       \
        return fn(*n, a, err);                                                         \
    }

SQL_NULL_VALUE_METHOD(sc_ns_value, SqlNullString, sql_null_string_value)
SQL_NULL_VALUE_METHOD(sc_n64_value, SqlNullInt64, sql_null_int64_value)
SQL_NULL_VALUE_METHOD(sc_n32_value, SqlNullInt32, sql_null_int32_value)
SQL_NULL_VALUE_METHOD(sc_n16_value, SqlNullInt16, sql_null_int16_value)
SQL_NULL_VALUE_METHOD(sc_nbyte_value, SqlNullByte, sql_null_byte_value)
SQL_NULL_VALUE_METHOD(sc_nf64_value, SqlNullFloat64, sql_null_float64_value)
SQL_NULL_VALUE_METHOD(sc_nbool_value, SqlNullBool, sql_null_bool_value)
SQL_NULL_VALUE_METHOD(sc_ntime_value, SqlNullTime, sql_null_time_value)
SQL_NULL_VALUE_METHOD(sc_null_value, SqlNull, sql_null_value)

#define SQL_NS_METHODS(M, T)                                                           \
    M(T, Scan, sql_null_string_scan, SQL_SIG_SCAN)                                     \
    M(T, Value, sc_ns_value, DRIVER_SIG_VALUE)
#define SQL_N64_METHODS(M, T)                                                          \
    M(T, Scan, sql_null_int64_scan, SQL_SIG_SCAN)                                      \
    M(T, Value, sc_n64_value, DRIVER_SIG_VALUE)
#define SQL_N32_METHODS(M, T)                                                          \
    M(T, Scan, sql_null_int32_scan, SQL_SIG_SCAN)                                      \
    M(T, Value, sc_n32_value, DRIVER_SIG_VALUE)
#define SQL_N16_METHODS(M, T)                                                          \
    M(T, Scan, sql_null_int16_scan, SQL_SIG_SCAN)                                      \
    M(T, Value, sc_n16_value, DRIVER_SIG_VALUE)
#define SQL_NBYTE_METHODS(M, T)                                                        \
    M(T, Scan, sql_null_byte_scan, SQL_SIG_SCAN)                                       \
    M(T, Value, sc_nbyte_value, DRIVER_SIG_VALUE)
#define SQL_NF64_METHODS(M, T)                                                         \
    M(T, Scan, sql_null_float64_scan, SQL_SIG_SCAN)                                    \
    M(T, Value, sc_nf64_value, DRIVER_SIG_VALUE)
#define SQL_NBOOL_METHODS(M, T)                                                        \
    M(T, Scan, sql_null_bool_scan, SQL_SIG_SCAN)                                       \
    M(T, Value, sc_nbool_value, DRIVER_SIG_VALUE)
#define SQL_NTIME_METHODS(M, T)                                                        \
    M(T, Scan, sql_null_time_scan, SQL_SIG_SCAN)                                       \
    M(T, Value, sc_ntime_value, DRIVER_SIG_VALUE)
#define SQL_NULL_METHODS(M, T)                                                         \
    M(T, Scan, sql_null_scan, SQL_SIG_SCAN)                                            \
    M(T, Value, sc_null_value, DRIVER_SIG_VALUE)

BURROW_METHODS_DEFINE(SqlNullString, SQL_NS_METHODS);
BURROW_METHODS_DEFINE(SqlNullInt64, SQL_N64_METHODS);
BURROW_METHODS_DEFINE(SqlNullInt32, SQL_N32_METHODS);
BURROW_METHODS_DEFINE(SqlNullInt16, SQL_N16_METHODS);
BURROW_METHODS_DEFINE(SqlNullByte, SQL_NBYTE_METHODS);
BURROW_METHODS_DEFINE(SqlNullFloat64, SQL_NF64_METHODS);
BURROW_METHODS_DEFINE(SqlNullBool, SQL_NBOOL_METHODS);
BURROW_METHODS_DEFINE(SqlNullTime, SQL_NTIME_METHODS);
BURROW_METHODS_DEFINE(SqlNull, SQL_NULL_METHODS);

#define SQL_NULL_TYPE(T, gonm, vname, vtype, member, h)                                \
    static const Field sc_fields_##T[] = {                                             \
        SQL_FIELD(vname, vtype, T, member),                                            \
        SQL_FIELD("Valid", TYPE_BOOL, T, valid),                                       \
    };                                                                                 \
    const Type burrow_type_##T = {                                                     \
        {(const Byte *)(gonm), (Int)(sizeof(gonm) - 1)},                               \
        {(const Byte *)"database/sql", 12},                                            \
        KIND_STRUCT,                                                                   \
        (uint32_t)sizeof(T),                                                           \
        (uint16_t)_Alignof(T),                                                         \
        2,                                                                             \
        2,                                                                             \
        sc_fields_##T,                                                                 \
        burrow__methods_##T,                                                           \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        (h),                                                                           \
        NULL,                                                                          \
    }

SQL_NULL_TYPE(SqlNullString, "NullString", "String", TYPE_STRING, string, 0x73716e73U);
SQL_NULL_TYPE(SqlNullInt64, "NullInt64", "Int64", TYPE_INT64, int64, 0x73716e36U);
SQL_NULL_TYPE(SqlNullInt32, "NullInt32", "Int32", TYPE_INT32, int32, 0x73716e33U);
SQL_NULL_TYPE(SqlNullInt16, "NullInt16", "Int16", TYPE_INT16, int16, 0x73716e31U);
SQL_NULL_TYPE(SqlNullByte, "NullByte", "Byte", TYPE_UINT8, byte, 0x73716e62U);
SQL_NULL_TYPE(SqlNullFloat64, "NullFloat64", "Float64", TYPE_FLOAT64, float64,
              0x73716e66U);
SQL_NULL_TYPE(SqlNullBool, "NullBool", "Bool", TYPE_BOOL, bool_, 0x73716e42U);
SQL_NULL_TYPE(SqlNullTime, "NullTime", "Time", TYPE_TIME, time, 0x73716e74U);
SQL_NULL_TYPE(SqlNull, "Null", "V", &burrow_type_Any, v, 0x73716e6cU);
