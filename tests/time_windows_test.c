/* Derived from Go's src/time/zoneinfo_windows_test.go.
 * Go source: go1.27.1.
 *
 * Go builds Local from Windows's TIME_ZONE_INFORMATION, and so does burrow,
 * but here the building is compiled on every system, so the tests that hand it
 * Go's usPacific and aus run everywhere. TestToEnglishName reads the registry
 * and runs on Windows only. The tests after it are not in Go: they check the
 * zones and transitions the two samples give, a zone with no daylight time,
 * and the capital letters a name not in the table falls back to.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/time/time_internal.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/pal.h"
#include "burrow/time.h"

#include <string.h>

static void tzi_name(char *out, int64_t *len, const char *s) {
    size_t n = strlen(s);
    memcpy(out, s, n);
    *len = (int64_t)n;
}

/* Go's usPacific and aus. */
static PalTzInfo us_pacific(void) {
    PalTzInfo i;
    memset(&i, 0, sizeof i);
    i.bias = 8 * 60;
    tzi_name(i.standard_name, &i.standard_name_len, "Pacific Standard Time");
    i.standard_date = (PalTzDate){.month = 11, .day = 1, .hour = 2};
    tzi_name(i.daylight_name, &i.daylight_name_len, "Pacific Daylight Time");
    i.daylight_date = (PalTzDate){.month = 3, .day = 2, .hour = 2};
    i.daylight_bias = -60;
    return i;
}

static PalTzInfo aus(void) {
    PalTzInfo i;
    memset(&i, 0, sizeof i);
    i.bias = -10 * 60;
    tzi_name(i.standard_name, &i.standard_name_len, "AUS Eastern Standard Time");
    i.standard_date = (PalTzDate){.month = 4, .day = 1, .hour = 3};
    tzi_name(i.daylight_name, &i.daylight_name_len, "AUS Eastern Daylight Time");
    i.daylight_date = (PalTzDate){.month = 10, .day = 1, .hour = 2};
    i.daylight_bias = -60;
    return i;
}

/* The location Local was last made from. Local keeps using its zones, so it
 * is freed only once another one takes its place. */
static TimeLocation *forced_local;

/* ForceUSPacificFromTZIForTesting and ForceAusFromTZIForTesting. */
static void force_local_from_tzi(TestingT *t, PalTzInfo i) {
    TimeLocation *l = burrow__time_location_from_tzi(heap_allocator(), &i);
    if (l == NULL)
        testing_t_fatalf_v(t, "initLocalFromTZI: out of memory");
    burrow__time_force_local(l);
    time_location_free(forced_local);
    forced_local = l;
}

static void test_zone_abbr(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Time t1 = time_now();
    /* discard nsec */
    t1 = time_date(time_year(t1), time_month(t1), time_day(t1), time_hour(t1),
                   time_minute(t1), time_second(t1), 0, time_location(t1));

    Error err = BURROW_NO_ERROR;
    Time t2 = time_parse(a, TIME_RFC1123, time_format(t1, a, TIME_RFC1123), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Parse failed: %v", err);
    if (!time_equal(t1, t2) || time_location(t1) != time_location(t2))
        testing_t_fatalf_v(t, "t1 (%s) is not equal to t2 (%s)",
                           time_format(t1, a, TIME_RFC1123),
                           time_format(t2, a, TIME_RFC1123));
    arena_free(&ar);
}

static void TestUSPacificZoneAbbr(TestingT *t) {
    force_local_from_tzi(t, us_pacific());
    test_zone_abbr(t);
}

static void TestAusZoneAbbr(TestingT *t) {
    force_local_from_tzi(t, aus());
    test_zone_abbr(t);
}

static void TestToEnglishName(TestingT *t) {
#if defined(BURROW_OS_WINDOWS)
    static const char want[] = "Central Europe Standard Time";
    char std[PAL_TZ_NAME_MAX], dlt[PAL_TZ_NAME_MAX], name[PAL_TZ_NAME_MAX];
    int64_t std_len = 0, dlt_len = 0, name_len = 0;
    PalErrno e = PAL_OK;
    if (!pal_tz_key_names(want, (int64_t)(sizeof want - 1), std, &std_len, dlt,
                          &dlt_len, &e))
        testing_t_fatalf_v(
            t, "cannot read CEST time zone information from registry: %d", (Int)e);
    if (!pal_tz_english_name(std, std_len, dlt, dlt_len, name, &name_len, &e))
        testing_t_fatalf_v(t, "toEnglishName failed: %d", (Int)e);
    Str got = {(const Byte *)name, name_len};
    if (!str_eq(got, str_from_cstr(want)))
        testing_t_fatalf_v(t, "english name: %q, want: %q", got, str_from_cstr(want));
#else
    testing_t_skip_v(t, "the registry is Windows only");
#endif
}

/* The zone Local has at the instant sec. */
static void check_zone(TestingT *t, int64_t sec, const char *name, Int offset) {
    Int off = 0;
    Str got = time_zone(time_in(time_from_unix(sec, 0), time_local_loc), &off);
    if (!str_eq(got, str_from_cstr(name)) || off != offset)
        testing_t_errorf_v(t, "zone at %d: %s %d, want %s %d", sec, got, off,
                           str_from_cstr(name), offset);
}

/* Unix second of a UTC date. */
static int64_t utc(Int y, TimeMonth m, Int d, Int h, Int min) {
    return time_unix(time_date(y, m, d, h, min, 0, 0, time_utc_loc));
}

/* usPacific is today's US rule, the second Sunday of March to the first of
 * November, at 2am local, and it is applied to every year near this one. */
static void TestUSPacificTransitions(TestingT *t) {
    force_local_from_tzi(t, us_pacific());
    Int y = time_year(time_now());
    for (Int year = y - 2; year <= y + 2; year++) {
        check_zone(t, utc(year, TIME_JANUARY, 15, 12, 0), "PST", -8 * 3600);
        check_zone(t, utc(year, TIME_JULY, 15, 12, 0), "PDT", -7 * 3600);
        check_zone(t, utc(year, TIME_DECEMBER, 15, 12, 0), "PST", -8 * 3600);
    }
    /* 2026: DST starts on March 8 at 2am PST, which is 10:00 UTC, and ends on
     * November 1 at 2am PDT, which is 09:00 UTC. */
    if (y - 100 <= 2026 && 2026 < y + 100) {
        check_zone(t, utc(2026, TIME_MARCH, 8, 9, 59), "PST", -8 * 3600);
        check_zone(t, utc(2026, TIME_MARCH, 8, 10, 0), "PDT", -7 * 3600);
        check_zone(t, utc(2026, TIME_NOVEMBER, 1, 8, 59), "PDT", -7 * 3600);
        check_zone(t, utc(2026, TIME_NOVEMBER, 1, 9, 0), "PST", -8 * 3600);
    }
}

/* aus has daylight time over the turn of the year, so its first change in a
 * year is to standard time. 2026: AEDT ends April 5 at 3am AEDT, which is
 * April 4 16:00 UTC, and starts October 4 at 2am AEST, October 3 16:00 UTC. */
static void TestAusTransitions(TestingT *t) {
    force_local_from_tzi(t, aus());
    check_zone(t, utc(2026, TIME_JANUARY, 15, 0, 0), "AEDT", 11 * 3600);
    check_zone(t, utc(2026, TIME_APRIL, 4, 15, 59), "AEDT", 11 * 3600);
    check_zone(t, utc(2026, TIME_APRIL, 4, 16, 0), "AEST", 10 * 3600);
    check_zone(t, utc(2026, TIME_JULY, 1, 0, 0), "AEST", 10 * 3600);
    check_zone(t, utc(2026, TIME_OCTOBER, 3, 15, 59), "AEST", 10 * 3600);
    check_zone(t, utc(2026, TIME_OCTOBER, 3, 16, 0), "AEDT", 11 * 3600);
}

/* A zone with no StandardDate has no daylight time, and its StandardBias is
 * not used. A name the table does not have gives its capital letters, which is
 * all that is left when the registry has no English name for it either. */
static void TestNoDaylightAndCaps(TestingT *t) {
    PalTzInfo i;
    memset(&i, 0, sizeof i);
    i.bias = -330;
    i.standard_bias = 60;
    tzi_name(i.standard_name, &i.standard_name_len, "Made Up Standard Time");
    tzi_name(i.daylight_name, &i.daylight_name_len, "Made Up Daylight Time");
    force_local_from_tzi(t, i);
    check_zone(t, utc(2026, TIME_JANUARY, 1, 0, 0), "MUST", 330 * 60);
    check_zone(t, utc(2026, TIME_JULY, 1, 0, 0), "MUST", 330 * 60);
    check_zone(t, utc(1900, TIME_JULY, 1, 0, 0), "MUST", 330 * 60);
    CHECK(str_eq(time_location_string(time_local_loc), BURROW_S("Local")));

    /* Not ASCII, and no capitals in it at all. */
    memset(&i, 0, sizeof i);
    tzi_name(i.standard_name, &i.standard_name_len, "\xc3\x89t\xc3\xa9");
    force_local_from_tzi(t, i);
    check_zone(t, utc(2026, TIME_JANUARY, 1, 0, 0), "", 0);
}

#define TESTS(X)                                                                       \
    X(TestUSPacificZoneAbbr)                                                           \
    X(TestAusZoneAbbr)                                                                 \
    X(TestToEnglishName)                                                               \
    X(TestUSPacificTransitions)                                                        \
    X(TestAusTransitions)                                                              \
    X(TestNoDaylightAndCaps)
TESTING_MAIN(TESTS)
