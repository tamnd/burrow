#include <stdint.h>
#include <stdio.h>

#include "burrow/burrow.h"

static void print_zone(const char *what, Time t) {
    Int off = 0;
    Str name = time_zone(t, &off);
    printf("%s: %.*s %+lld\n", what, (int)name.len, (const char *)name.p,
           (long long)off);
}

int main(void) {
    // doc: date
    Time t = time_date(2009, TIME_NOVEMBER, 10, 23, 0, 0, 0, time_utc_loc);
    TimeDateRet d = time_date_of(t);
    TimeClockRet c = time_clock(t);
    // doc: end
    printf("%lld-%02lld-%02lld %02lld:%02lld:%02lld unix %lld\n", (long long)d.year,
           (long long)d.month, (long long)d.day, (long long)c.hour, (long long)c.min,
           (long long)c.sec, (long long)time_unix(t));

    Str month = time_month_string(time_month(t), NULL);
    Str day = time_weekday_string(time_weekday(t), NULL);
    printf("%.*s, a %.*s, day %lld of the year\n", (int)month.len,
           (const char *)month.p, (int)day.len, (const char *)day.p,
           (long long)time_year_day(t));

    // doc: normalise
    Time n = time_date(2024, TIME_OCTOBER, 32, 0, 0, 0, 0, time_utc_loc);
    Time m = time_add_date(time_date(2024, TIME_JANUARY, 31, 0, 0, 0, 0, time_utc_loc),
                           0, 1, 0);
    // doc: end
    printf("October 32 is %02lld-%02lld, January 31 plus a month is %02lld-%02lld\n",
           (long long)time_month(n), (long long)time_day(n), (long long)time_month(m),
           (long long)time_day(m));

    // doc: zones
    Error err = BURROW_NO_ERROR;
    TimeLocation *ny = time_load_location(BURROW_S("America/New_York"), &err);
    if (ny == NULL)
        ny = time_fixed_zone(heap_allocator(), BURROW_S("EST"), -5 * 60 * 60);
    Time there = time_in(t, ny);
    // doc: end
    print_zone("New York", there);
    printf("same instant: %s\n", time_equal(there, t) ? "yes" : "no");

    // doc: arithmetic
    Time later = time_add(t, 90 * TIME_MINUTE);
    Duration gap = time_sub(later, t);
    Time hour = time_truncate(later, TIME_HOUR);
    // doc: end
    printf("gap %.1f minutes, truncated to %02lld:%02lld\n", duration_minutes(gap),
           (long long)time_hour(hour), (long long)time_minute(hour));

    // doc: elapsed
    Time start = time_now();
    time_sleep(2 * TIME_MILLISECOND);
    Duration took = time_since(start);
    // doc: end
    printf("took at least 2ms: %s\n", took >= 2 * TIME_MILLISECOND ? "yes" : "no");
    return 0;
}

/* Output:
2009-11-10 23:00:00 unix 1257894000
November, a Tuesday, day 314 of the year
October 32 is 11-01, January 31 plus a month is 03-02
New York: EST -18000
same instant: yes
gap 90.0 minutes, truncated to 00:00
took at least 2ms: yes
*/
