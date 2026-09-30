#include <stdint.h>
#include <stdio.h>

#include "burrow/burrow.h"

static void print_str(const char *what, Str s) {
    printf("%s: %.*s\n", what, (int)s.len, (const char *)s.p);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: format
    TimeLocation *ist = time_fixed_zone(a, BURROW_S("IST"), 5 * 60 * 60 + 30 * 60);
    Time t = time_date(2009, TIME_NOVEMBER, 10, 23, 4, 5, 123456789, time_utc_loc);
    Str rfc = time_format(t, a, TIME_RFC3339);
    Str there = time_format(time_in(t, ist), a, TIME_RFC1123);
    Str own = time_format(t, a, BURROW_S("Mon Jan _2 3:04PM, .000 seconds"));
    // doc: end
    print_str("RFC3339", rfc);
    print_str("RFC1123 in IST", there);
    print_str("own layout", own);
    print_str("String", time_string(t, a));

    // doc: parse
    Error err = BURROW_NO_ERROR;
    Time p = time_parse(a, TIME_RFC3339, BURROW_S("2006-01-02T15:04:05+07:00"), &err);
    Int off = 0;
    (void)time_zone(p, &off);
    // doc: end
    printf("parsed: unix %lld, offset %lld\n", (long long)time_unix(p), (long long)off);

    // doc: parse-error
    (void)time_parse(a, TIME_DATE_ONLY, BURROW_S("2024-02-30"), &err);
    // doc: end
    print_str("error", error_text(err));
    (void)time_parse(a, TIME_KITCHEN, BURROW_S("3:04 PM"), &err);
    const TimeParseError *pe = errors_as(err, TYPE_TIME_PARSE_ERROR);
    printf("layout element \"%.*s\", value element \"%.*s\"\n",
           (int)pe->layout_elem.len, (const char *)pe->layout_elem.p,
           (int)pe->value_elem.len, (const char *)pe->value_elem.p);

    // doc: json
    Slice j = time_marshal_json(t, a, &err);
    Time back = {0, 0, NULL};
    err = time_unmarshal_json(&back, a, j);
    // doc: end
    printf("JSON: %.*s, back equal: %s\n", (int)j.len, (const char *)j.p,
           time_equal(back, t) ? "yes" : "no");

    arena_free(&ar);
    return 0;
}

/* Output:
RFC3339: 2009-11-10T23:04:05Z
RFC1123 in IST: Wed, 11 Nov 2009 04:34:05 IST
own layout: Tue Nov 10 11:04PM, .123 seconds
String: 2009-11-10 23:04:05.123456789 +0000 UTC
parsed: unix 1136189045, offset 25200
error: parsing time "2024-02-30": day out of range
layout element "PM", value element " PM"
JSON: "2009-11-10T23:04:05.123456789Z", back equal: yes
*/
