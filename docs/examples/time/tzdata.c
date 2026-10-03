#include <stdint.h>
#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/time/tzdata.h"

int main(void) {
    // doc: register
    tzdata_register();

    Error err = BURROW_NO_ERROR;
    TimeLocation *tokyo = time_load_location(BURROW_S("Asia/Tokyo"), &err);
    if (tokyo == NULL) {
        fprintf(stderr, BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));
        return 1;
    }
    // doc: end

    Time t = time_date(2009, TIME_NOVEMBER, 10, 23, 0, 0, 0, time_utc_loc);
    Int off = 0;
    Str name = time_zone(time_in(t, tokyo), &off);
    printf("Tokyo: " BURROW_STR_FMT " %+lld, hour %lld\n", BURROW_STR_ARG(name),
           (long long)off, (long long)time_hour(time_in(t, tokyo)));

    TimeLocation *mars = time_load_location(BURROW_S("Mars/Olympus_Mons"), &err);
    printf("Mars: %s, " BURROW_STR_FMT "\n", mars == NULL ? "no" : "yes",
           BURROW_STR_ARG(error_text(err)));
    return 0;
}

/* Output:
Tokyo: JST +32400, hour 8
Mars: no, unknown time zone Mars/Olympus_Mons
*/
