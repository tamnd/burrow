#include <stdio.h>

#include "burrow/burrow.h"

// doc: valuer
/* A temperature that goes to the database as whole tenths of a degree. */
typedef struct Celsius {
    double degrees;
} Celsius;
extern const Type burrow_type_Celsius;

static Any celsius_value(Celsius *c, DriverAllocArg a, DriverErrorArg err) {
    if (c->degrees < -273.15) {
        BURROW_OUT(err, fmt_errorf_v("%v is below absolute zero", c->degrees));
        return (Any){NULL, NULL};
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    int64_t *tenths = BURROW_NEW(a, int64_t);
    if (tenths == NULL)
        return (Any){NULL, NULL};
    *tenths = (int64_t)(c->degrees * 10);
    return (Any){TYPE_INT64, tenths};
}

#define CELSIUS_FIELDS(F, T) F(T, double, degrees, "")
#define CELSIUS_METHODS(M, T) M(T, Value, celsius_value, DRIVER_SIG_VALUE)
BURROW_STRUCT_DEFINE_METHODS(Celsius, CELSIUS_FIELDS, CELSIUS_METHODS);
// doc: end

// doc: show
/* Runs one value through a converter and prints what comes out. */
static void show(DriverValueConverter c, Alloc *a, Any v) {
    Error err = BURROW_NO_ERROR;
    DriverValue out = c.vt->convert_value(c.data, a, v, &err);
    if (BURROW_FAILED(err))
        fmt_printf_v("%T %v: error: %v\n", v, v, err);
    else
        fmt_printf_v("%T %v -> %T %v\n", v, v, out, out);
}
// doc: end

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: default
    DriverValueConverter d = driver_default_parameter_converter;
    Int n = 7;
    uint8_t small = 200;
    float ratio = 0.5f;
    uint64_t huge = (uint64_t)1 << 63;
    show(d, a, BURROW_ANY(TYPE_INT, &n));
    show(d, a, BURROW_ANY(TYPE_UINT8, &small));
    show(d, a, BURROW_ANY(TYPE_FLOAT32, &ratio));
    show(d, a, BURROW_ANY(TYPE_UINT64, &huge));
    // doc: end

    // doc: callvaluer
    Celsius warm = {21.5}, wrong = {-300};
    show(d, a, BURROW_ANY(TYPE_OF(Celsius), &warm));
    show(d, a, BURROW_ANY(TYPE_OF(Celsius), &wrong));
    // doc: end

    // doc: bool
    Str yes = BURROW_S("TRUE"), maybe = BURROW_S("maybe");
    Int one = 1;
    show(driver_bool, a, BURROW_ANY(TYPE_STRING, &yes));
    show(driver_bool, a, BURROW_ANY(TYPE_INT, &one));
    show(driver_bool, a, BURROW_ANY(TYPE_STRING, &maybe));
    // doc: end

    // doc: result
    DriverRowsAffected affected = 3;
    DriverResult r = driver_rows_affected_as_result(&affected);
    Error err = BURROW_NO_ERROR;
    int64_t rows = r.vt->rows_affected(r.data, &err);
    fmt_printf_v("rows affected %d\n", rows);
    (void)r.vt->last_insert_id(r.data, &err);
    fmt_printf_v("last insert id: %v\n", err);
    // doc: end

    arena_free(&ar);
    return 0;
}

/* Output:
int 7 -> int64 7
uint8 200 -> int64 200
float32 0.5 -> float64 0.5
uint64 9223372036854775808: error: uint64 values with high bit set are not supported
Celsius {21.5} -> int64 215
Celsius {-300}: error: -300 is below absolute zero
string TRUE -> bool true
int 1 -> bool true
string maybe: error: sql/driver: couldn't convert "maybe" into type bool
rows affected 3
last insert id: LastInsertId is not supported by this driver
*/
