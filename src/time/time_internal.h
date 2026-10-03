/* What the files of package time share and nobody else sees: the inside of a
 * TimeLocation, the zone lookup, and the calendar arithmetic that Format and
 * Parse need as much as Date does.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_TIME_INTERNAL_H
#define BURROW_SRC_TIME_INTERNAL_H

#include "burrow/time.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"

#include <stdbool.h>
#include <stdint.h>

/* Go's zone: an abbreviation such as "CET", seconds east of UTC, and whether
 * it is daylight saving time. */
typedef struct TzZone {
    Str name;
    Int offset;
    bool is_dst;
} TzZone;

/* Go's zoneTrans: from when on, in seconds since 1970, zone index is in
 * effect. isstd and isutc are read and kept, and nothing uses them, which is
 * also true in Go. */
typedef struct TzTrans {
    int64_t when;
    uint8_t index;
    bool isstd;
    bool isutc;
} TzTrans;

/* Go's Location, field for field, with two additions for C.
 *
 * cache_own is where a zone that tzset works out for the cache lives when it
 * is not one of zone[], which Go gets by allocating a zone of its own. And a
 * and size say where the memory came from, so time_location_free can give it
 * back: a is NULL for the locations that live as long as the process does,
 * which are UTC, Local, the unnamed fixed zones and whatever LoadLocation
 * has handed out. next strings together the locations LoadLocation has
 * kept. */
struct TimeLocation {
    Str name;
    const TzZone *zone;
    Int nzone;
    const TzTrans *tx;
    Int ntx;
    Str extend;
    int64_t cache_start;
    int64_t cache_end;
    const TzZone *cache_zone;
    TzZone cache_own;
    Alloc *a;
    size_t size;
    struct TimeLocation *next;
};

enum {
    TZ_SECONDS_PER_MINUTE = 60,
    TZ_SECONDS_PER_HOUR = 60 * 60,
    TZ_SECONDS_PER_DAY = 24 * 60 * 60,
};

#define TZ_ALPHA INT64_MIN
#define TZ_OMEGA INT64_MAX

/* The seconds from year 1 to 1970, which is Go's unixToInternal, and the
 * other offsets Go's time.go spells out. */
#define TIME_ABSOLUTE_YEARS INT64_C(292277022400)
#define TIME_MARCH_THRU_DECEMBER 306
#define TIME_UNIX_TO_INTERNAL                                                          \
    ((int64_t)(1969 * 365 + 1969 / 4 - 1969 / 100 + 1969 / 400) * TZ_SECONDS_PER_DAY)
#define TIME_INTERNAL_TO_UNIX (-TIME_UNIX_TO_INTERNAL)
#define TIME_WALL_TO_INTERNAL                                                          \
    ((int64_t)(1884 * 365 + 1884 / 4 - 1884 / 100 + 1884 / 400) * TZ_SECONDS_PER_DAY)
/* -(absoluteYears*365.2425 + marchThruDecember) * secondsPerDay, done in
 * integers: 365.2425 years is 146097 days per 400 years. */
#define TIME_ABSOLUTE_TO_INTERNAL                                                      \
    (-(TIME_ABSOLUTE_YEARS / 400 * 146097 + TIME_MARCH_THRU_DECEMBER) *                \
     (int64_t)TZ_SECONDS_PER_DAY)
#define TIME_INTERNAL_TO_ABSOLUTE (-TIME_ABSOLUTE_TO_INTERNAL)
#define TIME_UNIX_TO_ABSOLUTE (TIME_UNIX_TO_INTERNAL + TIME_INTERNAL_TO_ABSOLUTE)
#define TIME_ABSOLUTE_TO_UNIX (-TIME_UNIX_TO_ABSOLUTE)

/* Go's Location.get: NULL is UTC, and Local is loaded the first time it is
 * looked at. */
TimeLocation *burrow__time_loc_get(TimeLocation *l);

/* The UTC location, which a Time spells as NULL. */
TimeLocation *burrow__time_utc(void);

/* What time/tzdata gives the loader, Go's loadFromEmbeddedTZData behind a
 * pointer the way Go's registerLoadFromEmbeddedTZData takes a func. load
 * returns false with no error when the name is not in the database. */
typedef struct TzEmbedded {
    bool (*load)(Str name, Str *data, Error *err);
} TzEmbedded;
void burrow__time_register_embedded(const TzEmbedded *e);

/* tzdata's own TzEmbedded, which a build with BURROW_TIMETZDATA uses without
 * being asked. */
extern const TzEmbedded burrow__tzdata_embedded;

/* For the tests, as Go's export_test.go has them: the embedded data for a
 * name, and the platform sources turned off and back on. */
bool burrow__time_load_from_embedded(Str name, Str *data, Error *err);
void burrow__time_disable_platform_sources(bool off);

/* Go's Location.lookup. */
typedef struct TzLookup {
    Str name;
    Int offset;
    int64_t start;
    int64_t end;
    bool is_dst;
} TzLookup;
TzLookup burrow__time_lookup(TimeLocation *l, int64_t sec);

/* Go's Location.lookupName: the offset of the zone called name at about unix
 * time unix, for Parse. */
bool burrow__time_lookup_name(TimeLocation *l, Str name, int64_t unix, Int *offset);

/* Go's tzset, exported for the tests the way Go's export_test.go does. */
bool burrow__time_tzset(Str s, int64_t last_tx_sec, int64_t sec, TzLookup *out);
bool burrow__time_tzset_name(Str s, Str *name, Str *rest);
bool burrow__time_tzset_offset(Str s, Int *offset, Str *rest);

typedef struct TzRule {
    int kind;
    Int day;
    Int week;
    Int mon;
    Int time;
} TzRule;
bool burrow__time_tzset_rule(Str s, TzRule *r, Str *rest);

/* The calendar, in Go's absolute days since March 1 of the year
 * -292277022399. */
typedef struct TimeYmd {
    Int year;
    TimeMonth month;
    Int day;
} TimeYmd;
TimeYmd burrow__time_abs_date(uint64_t days);
void burrow__time_abs_year_yday(uint64_t days, Int *year, Int *yday);
uint64_t burrow__time_date_to_abs_days(int64_t year, TimeMonth month, Int day);
uint64_t burrow__time_abs_sec(Time t);
bool burrow__time_is_leap(Int year);
Int burrow__time_days_in(TimeMonth m, Int year);
Int burrow__time_days_before(TimeMonth m);

/* The second since year 1, the nanosecond, and the unix second of t. */
int64_t burrow__time_sec(const Time *t);
int32_t burrow__time_nsec(const Time *t);
int64_t burrow__time_unix_sec(const Time *t);

/* Go's unixTime, Time.setLoc and Time.addSec. */
Time burrow__time_unix_time(int64_t sec, int32_t nsec);
void burrow__time_set_loc(Time *t, TimeLocation *loc);
void burrow__time_add_sec(Time *t, int64_t d);

/* Go's time.quote, which is not strconv.Quote: anything outside printable
 * ASCII comes out as \x escapes, byte by byte. Returns the length, and writes
 * only when out is not NULL, so it can be called once to measure and once to
 * fill. */
Int burrow__time_quote(Str s, Byte *out);

/* Go's parseTimeZone, exported for the tests the way export_test.go does. */
bool burrow__time_parse_time_zone(Str value, Int *length);

/* Go's ForceUSPacificForTesting, more or less: makes l the Local location,
 * under the name "Local", for a test that needs Local to be one place on every
 * machine. l has to outlive every use of Local. */
void burrow__time_force_local(TimeLocation *l);

/* Go's FixedZone for a caller that can not be handed NULL, such as the
 * binary decoding: the unnamed whole hour zones come from a table and the rest
 * from a. */
TimeLocation *burrow__time_fixed_zone(Alloc *a, Str name, Int offset);

#endif /* BURROW_SRC_TIME_INTERNAL_H */
