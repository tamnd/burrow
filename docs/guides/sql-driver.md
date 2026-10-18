# SQL drivers

`burrow/database/sql/driver.h` is Go's `database/sql/driver` package. It is the set of interfaces a database driver fills in, and a few helpers that drivers and `database/sql` share. Most programs never include it directly. A program that talks to a database uses `database/sql`, and only a driver, or a type that wants to say how it is stored, needs this header.

## Interfaces

Each of Go's interfaces is a vtable and a handle, the way every interface in the library is. A driver fills in `DriverVT`, `DriverConnVT`, `DriverStmtVT`, `DriverRowsVT` and `DriverTxVT`, and `database/sql` calls them. Go's optional interfaces, such as `ExecerContext`, `Pinger` and `SessionResetter`, are optional slots in the same vtable rather than separate types, so a driver leaves a slot NULL where Go's driver would not implement the method. The names Go gives them are typedefs of the vtable they live in, so `DriverExecerContextVT` is `DriverConnVT`.

A value crossing the boundary is a `DriverValue`, which is an `Any` holding nil, an `int64_t`, a `double`, a `bool`, `Bytes`, a `Str` or a `Time`, the same six types Go allows, or a decimal type with a `Decompose` method.

For a whole driver over a real C library, see [`examples/sqlite`](../../examples/sqlite/README.md). It wraps the system's SQLite in about 650 lines, fills in the context slots, named parameters, read only transactions and column type names, and comes with its own tests and a demo.

## Converting parameters

`driver_default_parameter_converter` is what `database/sql` uses on a query's arguments when the driver does not convert them itself. Each integer type becomes an `int64_t`, a `float` becomes a `double`, a named string or byte slice type becomes the plain one, and a pointer is followed, with a NULL pointer meaning SQL's NULL. A `uint64_t` with the high bit set is an error, because it does not fit:

<!-- example: ../examples/database/driver.c#show -->
```c
/* Runs one value through a converter and prints what comes out. */
static void show(DriverValueConverter c, Alloc *a, Any v) {
    Error err = BURROW_NO_ERROR;
    DriverValue out = c.vt->convert_value(c.data, a, v, &err);
    if (BURROW_FAILED(err))
        fmt_printf_v("%T %v: error: %v\n", v, v, err);
    else
        fmt_printf_v("%T %v -> %T %v\n", v, v, out, out);
}
```

<!-- example: ../examples/database/driver.c#default -->
```c
DriverValueConverter d = driver_default_parameter_converter;
Int n = 7;
uint8_t small = 200;
float ratio = 0.5f;
uint64_t huge = (uint64_t)1 << 63;
show(d, a, BURROW_ANY(TYPE_INT, &n));
show(d, a, BURROW_ANY(TYPE_UINT8, &small));
show(d, a, BURROW_ANY(TYPE_FLOAT32, &ratio));
show(d, a, BURROW_ANY(TYPE_UINT64, &huge));
```

That prints:

```text
int 7 -> int64 7
uint8 200 -> int64 200
float32 0.5 -> float64 0.5
uint64 9223372036854775808: error: uint64 values with high bit set are not supported
```

## Valuer

A type that should be stored as something other than what it is has a `Value` method, which is Go's `driver.Valuer`. In C the method goes in the type's method set with `BURROW_STRUCT_DEFINE_METHODS`, with `DRIVER_SIG_VALUE` as its signature. The converter finds it by name, calls it with the allocator the result should come from, and checks that what comes back is one of the six types:

<!-- example: ../examples/database/driver.c#valuer -->
```c
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
```

<!-- example: ../examples/database/driver.c#callvaluer -->
```c
Celsius warm = {21.5}, wrong = {-300};
show(d, a, BURROW_ANY(TYPE_OF(Celsius), &warm));
show(d, a, BURROW_ANY(TYPE_OF(Celsius), &wrong));
```

That prints:

```text
Celsius {21.5} -> int64 215
Celsius {-300}: error: -300 is below absolute zero
```

A NULL pointer to a type with a `Value` method converts to nil without the method being called, as in Go.

## The other converters

`driver_bool`, `driver_int32` and `driver_string` are Go's `Bool`, `Int32` and `String`, for a driver whose statement knows what type a column holds. `driver_bool` takes what `strconv_parse_bool` takes, and the integers 0 and 1:

<!-- example: ../examples/database/driver.c#bool -->
```c
Str yes = BURROW_S("TRUE"), maybe = BURROW_S("maybe");
Int one = 1;
show(driver_bool, a, BURROW_ANY(TYPE_STRING, &yes));
show(driver_bool, a, BURROW_ANY(TYPE_INT, &one));
show(driver_bool, a, BURROW_ANY(TYPE_STRING, &maybe));
```

That prints:

```text
string TRUE -> bool true
int 1 -> bool true
string maybe: error: sql/driver: couldn't convert "maybe" into type bool
```

`DriverNull` and `DriverNotNull` wrap another converter. The first lets nil through and the second turns it into an error.

## Results

`DriverRowsAffected` is the `Result` for a statement that knows how many rows it changed and has no insert ID, and `driver_result_no_rows` is the one for a statement that changed none, such as a `CREATE TABLE`:

<!-- example: ../examples/database/driver.c#result -->
```c
DriverRowsAffected affected = 3;
DriverResult r = driver_rows_affected_as_result(&affected);
Error err = BURROW_NO_ERROR;
int64_t rows = r.vt->rows_affected(r.data, &err);
fmt_printf_v("rows affected %d\n", rows);
(void)r.vt->last_insert_id(r.data, &err);
fmt_printf_v("last insert id: %v\n", err);
```

That prints:

```text
rows affected 3
last insert id: LastInsertId is not supported by this driver
```
