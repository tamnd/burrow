/* Derived from Go's src/database/sql/driver/driver.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/database/sql/driver.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/type.h"

#include <stddef.h>

/* Go's driver.go is the interfaces, the three errors and the two Results.
 * The interfaces are all in the header. */

BURROW_SENTINEL_ERROR(driver_err_skip,
                      "driver: skip fast-path; continue as if unimplemented");
BURROW_SENTINEL_ERROR(driver_err_bad_conn, "driver: bad connection");
BURROW_SENTINEL_ERROR(driver_err_remove_argument, "driver: remove argument from query");

/* ------------------------------------------------------------ NamedValue */

static const Field dr_named_value_fields[] = {
    {{(const Byte *)"Name", 4},
     {NULL, 0},
     TYPE_OF(Str),
     (uint32_t)offsetof(DriverNamedValue, name)},
    {{(const Byte *)"Ordinal", 7},
     {NULL, 0},
     TYPE_OF(Int),
     (uint32_t)offsetof(DriverNamedValue, ordinal)},
    {{(const Byte *)"Value", 5},
     {NULL, 0},
     TYPE_OF(Any),
     (uint32_t)offsetof(DriverNamedValue, value)},
};

const Type burrow_type_DriverNamedValue = {
    {(const Byte *)"NamedValue", 10},
    {(const Byte *)"database/sql/driver", 19},
    KIND_STRUCT,
    (uint32_t)sizeof(DriverNamedValue),
    (uint16_t)_Alignof(DriverNamedValue),
    3,
    0,
    dr_named_value_fields,
    NULL,
    NULL,
    NULL,
    0,
    0x64726e76U, /* "drnv" */
    NULL,
};

/* ------------------------------------------------------------ RowsAffected */

BURROW_SENTINEL_ERROR(dr_err_no_insert_id,
                      "LastInsertId is not supported by this driver");

int64_t driver_rows_affected_last_insert_id(DriverRowsAffected v, Error *err) {
    (void)v;
    BURROW_OUT(err, dr_err_no_insert_id);
    return 0;
}

int64_t driver_rows_affected_rows_affected(DriverRowsAffected v, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    return v;
}

static const Type dr_rows_affected_desc = {
    {(const Byte *)"RowsAffected", 12},
    {(const Byte *)"database/sql/driver", 19},
    KIND_INT64,
    (uint32_t)sizeof(DriverRowsAffected),
    (uint16_t)_Alignof(DriverRowsAffected),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x64727261U, /* "drra" */
    NULL,
};

static int64_t dr_rows_affected_last_insert_id(void *self, Error *err) {
    return driver_rows_affected_last_insert_id(*(DriverRowsAffected *)self, err);
}

static int64_t dr_rows_affected_rows_affected(void *self, Error *err) {
    return driver_rows_affected_rows_affected(*(DriverRowsAffected *)self, err);
}

static const DriverResultVT dr_rows_affected_vt = {
    &dr_rows_affected_desc,
    dr_rows_affected_last_insert_id,
    dr_rows_affected_rows_affected,
};

DriverResult driver_rows_affected_as_result(DriverRowsAffected *v) {
    return (DriverResult){&dr_rows_affected_vt, v};
}

/* ------------------------------------------------------------ ResultNoRows */

BURROW_SENTINEL_ERROR(dr_err_no_rows_insert_id,
                      "no LastInsertId available after DDL statement");
BURROW_SENTINEL_ERROR(dr_err_no_rows_affected,
                      "no RowsAffected available after DDL statement");

static const Type dr_no_rows_desc = {
    {(const Byte *)"noRows", 6},
    {(const Byte *)"database/sql/driver", 19},
    KIND_STRUCT,
    0,
    1,
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x64726e72U, /* "drnr" */
    NULL,
};

static int64_t dr_no_rows_last_insert_id(void *self, Error *err) {
    (void)self;
    BURROW_OUT(err, dr_err_no_rows_insert_id);
    return 0;
}

static int64_t dr_no_rows_rows_affected(void *self, Error *err) {
    (void)self;
    BURROW_OUT(err, dr_err_no_rows_affected);
    return 0;
}

static const DriverResultVT dr_no_rows_vt = {
    &dr_no_rows_desc,
    dr_no_rows_last_insert_id,
    dr_no_rows_rows_affected,
};

const DriverResult driver_result_no_rows = {&dr_no_rows_vt, NULL};
