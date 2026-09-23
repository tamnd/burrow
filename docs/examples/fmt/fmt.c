#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/heap.h"

// doc: types
#define POINT_FIELDS(F, T)                                                             \
    F(T, Int, X, "")                                                                   \
    F(T, Int, Y, "")
BURROW_STRUCT(Point, POINT_FIELDS);
// doc: end

// doc: stringer
#define CELSIUS_FIELDS(F, T) F(T, double, Deg, "")
BURROW_STRUCT_DECL(Celsius, CELSIUS_FIELDS);

static Str celsius_string(Celsius *c) {
    return fmt_sprintf_v(heap_allocator(), "%.1f°C", c->Deg);
}

#define CELSIUS_SIG_String(IN, OUT) OUT(Str)
#define CELSIUS_METHODS(M, T) M(T, String, celsius_string, CELSIUS_SIG_String)
BURROW_STRUCT_DEFINE_METHODS(Celsius, CELSIUS_FIELDS, CELSIUS_METHODS);
// doc: end

// doc: scanner
#define RGB_FIELDS(F, T)                                                               \
    F(T, uint8_t, R, "")                                                               \
    F(T, uint8_t, G, "")                                                               \
    F(T, uint8_t, B, "")
BURROW_STRUCT_DECL(Rgb, RGB_FIELDS);

static Error rgb_scan(Rgb *c, FmtScanState st, Rune verb) {
    (void)verb;
    Error err = BURROW_NO_ERROR;
    fmt_fscanf_v(heap_allocator(), &err, fmt_scan_state_reader(&st), "#%2x%2x%2x",
                 &c->R, &c->G, &c->B);
    return err;
}

#define RGB_SIG_Scan(IN, OUT) IN(0, FmtScanState) IN(1, Rune) OUT(Error)
#define RGB_METHODS(M, T) M(T, Scan, rgb_scan, RGB_SIG_Scan)
BURROW_STRUCT_DEFINE_METHODS(Rgb, RGB_FIELDS, RGB_METHODS);
// doc: end

BURROW_SENTINEL_ERROR(err_not_found, "not found");

static void printing(void) {
    // doc: printf
    fmt_printf_v("%d items at %.2f each, %q\n", 3, 1.5, "tea");
    // doc: end

    // doc: println
    bool done = true;
    fmt_println_v("sum:", 2 + 2, done, 0.5);
    // doc: end
}

static void strings(Alloc *a) {
    // doc: sprintf
    Str s = fmt_sprintf_v(a, "[%6.2f|%-5s|%#x|%08b]", 3.14159, "ab", 255, 5);
    // doc: end
    fmt_println_v(s);

    // doc: slice
    Slice line = fmt_appendf_v(a, slice_nil(TYPE_BYTE), "id=%d", 42);
    line = fmt_append_v(a, line, " ok");
    // doc: end
    printf("%.*s\n", (int)line.len, (const char *)line.p);

    // doc: explicit
    Any args[] = {BURROW_ANY_OF(7), BURROW_ANY_OF("seven")};
    Str t =
        fmt_sprintf(a, BURROW_S("%[2]s is %[1]d"), slice_from(args, 2, 2, TYPE_ANY));
    // doc: end
    fmt_println_v(t);
}

static void values(Alloc *a) {
    // doc: struct
    Point p = {1, 2};
    Any v = BURROW_ANY(TYPE_OF(Point), &p);
    fmt_printf_v("%v %+v %#v %T\n", v, v, v, v);
    // doc: end

    // doc: map
    Map *m = map_make(a, TYPE_STRING, TYPE_INT, 0);
    BURROW_MAP_SET(Str, Int, m, BURROW_S("b"), 2);
    BURROW_MAP_SET(Str, Int, m, BURROW_S("a"), 1);
    Int xs[] = {3, 1, 4};
    fmt_println_v(m, slice_from(xs, 3, 3, TYPE_INT));
    // doc: end

    // doc: method
    Celsius c = {21.5};
    fmt_printf_v("it is %v, or %d in the raw\n", BURROW_ANY(TYPE_OF(Celsius), &c),
                 BURROW_ANY(TYPE_OF(Celsius), &c));
    // doc: end
}

static void mistakes(Alloc *a) {
    // doc: bad
    Str s = fmt_sprintf_v(a, "%d %s|%d", "x", 5);
    // doc: end
    fmt_println_v(s);
}

static void errors(void) {
    // doc: errorf
    Error err = fmt_errorf_v("load %q: %w", "config.toml", err_not_found);
    if (errors_is(err, err_not_found))
        fmt_println_v(err);
    // doc: end
}

static void scanning(Alloc *a) {
    // doc: sscan
    Int n = 0;
    Str item = {0};
    double price = 0;
    Error err = BURROW_NO_ERROR;
    Int got = fmt_sscan_v(a, &err, "3 tea\n1.5", &n, &item, &price);
    // doc: end
    fmt_println_v(got, n, item, price);

    // doc: sscanf
    int h = 0, m = 0;
    fmt_sscanf_v(a, &err, "at 09:45", "at %d:%d", &h, &m);
    // doc: end
    fmt_println_v(h, m);

    // doc: scanerr
    Int x = 0;
    if (fmt_sscan_v(a, &err, "ten", &x) != 1)
        fmt_println_v(err);
    // doc: end

    // doc: scan-method
    Rgb c = {0};
    fmt_sscan_v(a, &err, "#ff8000", BURROW_ANY(TYPE_OF(Rgb), &c));
    fmt_println_v(BURROW_ANY(TYPE_OF(Rgb), &c));
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);

    printing();
    strings(a);
    values(a);
    mistakes(a);
    errors();
    scanning(a);

    arena_free(&ar);
    return 0;
}

/* Output:
3 items at 1.50 each, "tea"
sum: 4 true 0.5
[  3.14|ab   |0xff|00000101]
id=42 ok
seven is 7
{1 2} {X:1 Y:2} Point{X:1, Y:2} Point
map[a:1 b:2] [3 1 4]
it is 21.5°C, or {%!d(float64=21.5)} in the raw
%!d(string=x) %!s(int=5)|%!d(MISSING)
load "config.toml": not found
3 3 tea 1.5
9 45
expected integer
{255 128 0}
*/
