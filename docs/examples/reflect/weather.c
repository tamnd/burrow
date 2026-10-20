#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/encoding/json.h"
#include "burrow/mem/arena.h"

// doc: include
#include "weather_gen.h"

bool weather_reading_below(WeatherReading r, WeatherCelsius t) {
    return r.temp < t;
}
// doc: end

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: use
    Str tags[] = {BURROW_S("roof")};
    WeatherReading r = {BURROW_S("north"), -2.5, slice_from(tags, 1, 1, TYPE_OF(Str))};
    Error err = BURROW_NO_ERROR;
    Slice out = json_marshal(a, BURROW_ANY(TYPE_OF(WeatherReading), &r), &err);
    printf("%.*s\n", (int)out.len, (const char *)out.p);
    printf("below freezing: %d\n", weather_reading_below(r, WEATHER_FREEZING));
    const Type *c = TYPE_OF(WeatherCelsius);
    printf(BURROW_STR_FMT " is a " BURROW_STR_FMT "\n", BURROW_STR_ARG(c->name),
           BURROW_STR_ARG(kind_name(c->kind)));
    // doc: end

    arena_free(&ar);
    return 0;
}

/* Output:
{"sensor":"north","temp":-2.5,"tags":["roof"]}
below freezing: 1
WeatherCelsius is a float64
*/
