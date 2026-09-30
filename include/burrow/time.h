/* Sleeping, and running a function later on.
 *
 * Go's time package: Duration and its constants, the Time type with its wall
 * clock and its monotonic reading, the calendar, locations and the zoneinfo
 * database, Format and Parse, time.Sleep, and the timers: AfterFunc, NewTimer,
 * After, NewTicker and Tick, with the Stop and Reset that go with them.
 *
 * A goroutine that sleeps here costs a timer and no thread. The thread it was
 * running on goes and finds other work, and one of the scheduler's threads wakes
 * it again when the time is up, which is why a program can have a hundred
 * thousand goroutines waiting on a hundred thousand deadlines and still be
 * asleep in the kernel using no processor at all. That is the whole reason the
 * per P timer heaps in burrow/timer.h exist, and this header is the part of it
 * people actually call.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package time */

#ifndef BURROW_TIME_H
#define BURROW_TIME_H

#include "burrow/chan.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/platform.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A length of time in nanoseconds, which is Go's time.Duration exactly.
 *
 * Signed, so a difference between two readings can be negative, and 64 bits
 * wide, which puts the largest duration at about 292 years. Anything that needs
 * longer than that is a date and not a duration.
 *
 * It is a plain integer and not a struct on purpose. Go's is a plain int64 too,
 * so d / 2, d * 3 and d1 < d2 all mean what they look like, and a Duration can
 * be printed with the format string for a long long. */
typedef int64_t Duration;

/* The units, which are Go's constants and are used the same way.
 *
 *     time_sleep(500 * TIME_MILLISECOND);
 *     time_sleep(2 * TIME_SECOND + 300 * TIME_MILLISECOND);
 *
 * Multiplying by the unit rather than writing the nanoseconds out is not a
 * style preference. 500000000 and 5000000000 look the same at a glance and one
 * of them is ten times the other, and that is a real bug that has shipped in
 * real programs more than once. */
#define TIME_NANOSECOND ((Duration)1)
#define TIME_MICROSECOND (1000 * TIME_NANOSECOND)
#define TIME_MILLISECOND (1000 * TIME_MICROSECOND)
#define TIME_SECOND (1000 * TIME_MILLISECOND)
#define TIME_MINUTE (60 * TIME_SECOND)
#define TIME_HOUR (60 * TIME_MINUTE)

/* time.ParseDuration. A duration string is an optional sign and then one or
 * more numbers, each with an optional fraction and a unit, such as "300ms",
 * "-1.5h" or "2h45m". The units are "ns", "us" (or "µs"), "ms", "s", "m" and
 * "h". A bare "0" is fine without a unit.
 *
 *     Duration d = time_parse_duration(BURROW_S("1h15m30.918273645s"), &err);
 *
 * On failure it returns 0 and sets *err to an error whose text is Go's, such
 * as time: invalid duration "1d". The error lives in the calling goroutine's
 * error arena. err may be NULL. */
Duration time_parse_duration(Str s, Error *err);

/* time.Duration.String. The shortest form that reads back to the same value,
 * such as "72h3m0.5s". Anything under a second uses a smaller unit, so 1.5ms
 * comes out as "1.5ms", and zero is "0s". The longest result is 25 bytes. */
BURROW_OWNS(ret) Str duration_string(Duration d, Alloc *a);

/* The same text written into buf, which needs room for
 * DURATION_STRING_MAX bytes. Returns the length, and allocates
 * nothing. */
enum { DURATION_STRING_MAX = 32 };
Int duration_format(Duration d, Byte *buf);

/* The descriptor for Duration, a named int64 in package time with a String
 * method, which is what lets fmt's %v print "1.5s" rather than 1500000000 and
 * flag tell a duration flag from an int64 one. String has no allocator to
 * take, so it puts its text in the calling goroutine's error arena. */
extern const Type burrow_type_Duration;
#define TYPE_DURATION TYPE_OF(Duration)

/* Go's Duration methods. Nanoseconds, Microseconds and Milliseconds are the
 * count as an integer, truncated towards zero. Seconds, Minutes and Hours are
 * a float64, worked out in two halves so that a long duration keeps its
 * fraction. */
int64_t duration_nanoseconds(Duration d);
int64_t duration_microseconds(Duration d);
int64_t duration_milliseconds(Duration d);
double duration_seconds(Duration d);
double duration_minutes(Duration d);
double duration_hours(Duration d);

/* d rounded towards zero to a multiple of m, and d rounded to the nearest
 * multiple of m with halfway cases away from zero. An m of zero or less gives
 * d back. A Round that would overflow gives the largest or smallest Duration
 * instead, which is Go's rule. */
Duration duration_truncate(Duration d, Duration m);
Duration duration_round(Duration d, Duration m);

/* The absolute value of d, with the smallest Duration, which has no positive
 * twin, coming back as the largest. */
Duration duration_abs(Duration d);

/* ------------------------------------------------------------------ Time */

/* A month of the year, January being 1. Go's time.Month. */
typedef Int TimeMonth;

#define TIME_JANUARY ((TimeMonth)1)
#define TIME_FEBRUARY ((TimeMonth)2)
#define TIME_MARCH ((TimeMonth)3)
#define TIME_APRIL ((TimeMonth)4)
#define TIME_MAY ((TimeMonth)5)
#define TIME_JUNE ((TimeMonth)6)
#define TIME_JULY ((TimeMonth)7)
#define TIME_AUGUST ((TimeMonth)8)
#define TIME_SEPTEMBER ((TimeMonth)9)
#define TIME_OCTOBER ((TimeMonth)10)
#define TIME_NOVEMBER ((TimeMonth)11)
#define TIME_DECEMBER ((TimeMonth)12)

/* A day of the week, Sunday being 0. Go's time.Weekday. */
typedef Int TimeWeekday;

#define TIME_SUNDAY ((TimeWeekday)0)
#define TIME_MONDAY ((TimeWeekday)1)
#define TIME_TUESDAY ((TimeWeekday)2)
#define TIME_WEDNESDAY ((TimeWeekday)3)
#define TIME_THURSDAY ((TimeWeekday)4)
#define TIME_FRIDAY ((TimeWeekday)5)
#define TIME_SATURDAY ((TimeWeekday)6)

/* The English name, "January" or "Sunday". A value out of range comes out the
 * way Go prints it, as "%!Month(13)", and only that case uses a, so a caller
 * who knows the value is in range may pass NULL. */
BURROW_OWNS(ret) Str time_month_string(TimeMonth m, Alloc *a);
BURROW_OWNS(ret) Str time_weekday_string(TimeWeekday d, Alloc *a);

extern const Type burrow_type_TimeMonth;
extern const Type burrow_type_TimeWeekday;
#define TYPE_TIME_MONTH TYPE_OF(TimeMonth)
#define TYPE_TIME_WEEKDAY TYPE_OF(TimeWeekday)

/* A time zone, or more exactly the rules for one place: the offsets it has
 * used and when it changed between them. Go's time.Location.
 *
 * Opaque, as Go's is. A TimeLocation * from time_load_location, and the two
 * below, live as long as the process and are never freed. One from
 * time_fixed_zone or time_load_location_from_tz_data belongs to the caller,
 * who gives it back with time_location_free once no Time uses it. */
typedef struct TimeLocation TimeLocation;

/* Go's time.UTC and time.Local. They are constants here rather than variables
 * a program can point somewhere else, because a variable every thread reads
 * and any thread may write is a race, which it is in Go as well. Local is what
 * the TZ environment variable says, or /etc/localtime when it says nothing,
 * and is loaded the first time something uses it. On Windows it is UTC for
 * now, until the registry lookup lands. */
extern TimeLocation *const time_utc_loc;
extern TimeLocation *const time_local_loc;

/* The name the location was loaded under, such as "Europe/Paris", "UTC" or
 * "Local". Borrowed from the location. */
BURROW_BORROWS(ret, l) Str time_location_string(TimeLocation *l);

/* A location that is always offset seconds east of UTC and always calls
 * itself name. Go's time.FixedZone. An unnamed zone at a whole number of
 * hours between -12 and +14 comes from a table and costs nothing, and anything
 * else comes from a, with a copy of name. NULL when a is out of memory. */
BURROW_OWNS(ret) TimeLocation *time_fixed_zone(Alloc *a, Str name, Int offset);

/* The location called name. Go's time.LoadLocation.
 *
 * "" and "UTC" are UTC and "Local" is Local. Anything else is a name in the
 * IANA database, such as "America/New_York", looked for first under the
 * directory or zip file $ZONEINFO names, then in the system's zoneinfo
 * directory. A name with ".." in it, or that starts with a slash, is refused.
 *
 * Each name is read once and kept, so two loads of one name are the same
 * pointer and neither needs freeing. On failure it returns NULL and sets *err
 * to Go's error, such as unknown time zone Mars/Olympus_Mons. */
BURROW_BORROWS(ret) TimeLocation *time_load_location(Str name, Error *err);

/* A location made from the bytes of a TZif file, the format of the zoneinfo
 * database. Go's time.LoadLocationFromTZData. The location copies what it
 * needs from data and comes from a. On bad data it returns NULL with *err set
 * to malformed time zone information. */
BURROW_OWNS(ret) TimeLocation *time_load_location_from_tz_data(Alloc *a, Str name,
                                                               Slice data, Error *err);

/* Gives back a location from time_fixed_zone or
 * time_load_location_from_tz_data. Any other location, and NULL, is left
 * alone, so this is safe to call on whatever time_location returned. */
void time_location_free(TimeLocation *l);

/* An instant in time with nanosecond precision. Go's time.Time.
 *
 * A value type, passed and returned by value, and the zero value is January
 * 1, year 1, 00:00:00 UTC, which time_is_zero recognises. The fields are Go's
 * and are laid out the same way, and are not for touching: wall holds the
 * nanosecond and, for a reading from time_now, the wall clock second as well
 * as a flag saying ext is a monotonic reading; otherwise ext is the second
 * since year 1. loc is NULL for UTC.
 *
 * A Time from time_now carries the monotonic clock with it, and time_sub,
 * time_since, time_until and the comparisons use that reading when both sides
 * have one, so an elapsed time is right even across somebody setting the
 * system clock. Anything that makes a new instant rather than moving along the
 * clock drops it: time_utc, time_in, time_truncate, time_round, time_add_date
 * and time_date.
 *
 * Compare two Times with time_equal rather than memcmp. Two readings of one
 * instant can differ in loc and in the monotonic reading. */
typedef struct Time {
    uint64_t wall;
    int64_t ext;
    TimeLocation *loc;
} Time;

/* The current local time, with a monotonic reading. Go's time.Now. Inside a
 * synctest bubble this is the bubble's fake clock, which starts at midnight UTC
 * on 2000-01-01. */
Time time_now(void);

/* The local Time for sec seconds and nsec nanoseconds since the unix epoch.
 * nsec may be outside [0, 999999999], and carries into sec. Go's time.Unix.
 * The name has from in it because time_unix is Go's Time.Unix, the other
 * direction. */
Time time_from_unix(int64_t sec, int64_t nsec);

/* The same from milliseconds and from microseconds since the epoch. Go's
 * time.UnixMilli and time.UnixMicro. */
Time time_from_unix_milli(int64_t msec);
Time time_from_unix_micro(int64_t usec);

/* The Time that is year-month-day hour:min:sec + nsec nanoseconds in loc.
 * Go's time.Date.
 *
 * Every field may be out of its range and carries into the next, so October
 * 32 is November 1. A local time that happens twice, in the hour a clock goes
 * back, comes out as the first; one that never happens, in the hour a clock
 * goes forward, comes out an hour on. Both are what Go does, and Go does not
 * promise which way the ambiguous ones fall.
 *
 *     Time t = time_date(2009, TIME_NOVEMBER, 10, 23, 0, 0, 0, time_utc_loc);
 *
 * A NULL loc panics, with Go's message. */
Time time_date(Int year, TimeMonth month, Int day, Int hour, Int min, Int sec, Int nsec,
               TimeLocation *loc);

/* The time elapsed since t, and the time until t. Go's time.Since and
 * time.Until, which use the monotonic reading when t has one. */
Duration time_since(Time t);
Duration time_until(Time t);

/* t + d, keeping the monotonic reading moving with it. Go's Time.Add. */
Time time_add(Time t, Duration d);

/* t - u. A result too big for a Duration is the largest or smallest one.
 * Go's Time.Sub. */
Duration time_sub(Time t, Time u);

/* The Time years, months and days after t, normalised the way time_date
 * normalises, so October 31 plus a month is December 1. Go's Time.AddDate. */
Time time_add_date(Time t, Int years, Int months, Int days);

/* Whether t is after u, before u, or the same instant, and -1, 0 or +1 in
 * the order of the two. Go's Time.After, Before, Equal and Compare. The
 * location plays no part: 6:00 +0200 and 4:00 UTC are equal. */
bool time_after(Time t, Time u);
bool time_before(Time t, Time u);
bool time_equal(Time t, Time u);
Int time_compare(Time t, Time u);

/* Whether t is the zero Time, January 1, year 1, 00:00:00 UTC. */
bool time_is_zero(Time t);

/* The pieces of t, in t's location. Go's Time.Date and Time.Clock, which
 * return three things each. Date takes the name time_date_of because
 * time_date is the function that builds a Time. */
typedef struct TimeDateRet {
    Int year;
    TimeMonth month;
    Int day;
} TimeDateRet;
TimeDateRet time_date_of(Time t);

typedef struct TimeClockRet {
    Int hour;
    Int min;
    Int sec;
} TimeClockRet;
TimeClockRet time_clock(Time t);

Int time_year(Time t);
TimeMonth time_month(Time t);
Int time_day(Time t);
TimeWeekday time_weekday(Time t);
Int time_hour(Time t);
Int time_minute(Time t);
Int time_second(Time t);
Int time_nanosecond(Time t);

/* The day of the year, 1 to 365, or 366 in a leap year. */
Int time_year_day(Time t);

/* The ISO 8601 year and week number. Weeks start on Monday and week 1 is the
 * one with the year's first Thursday in it, so January 1 to 3 can be in the
 * last week of the year before and December 29 to 31 in week 1 of the next.
 * week may be NULL. */
Int time_iso_week(Time t, Int *week);

/* t in UTC, in Local and in loc. The instant is the same and only what the
 * accessors report changes. A NULL loc panics. */
Time time_utc(Time t);
Time time_local(Time t);
Time time_in(Time t, TimeLocation *loc);

/* t's location, which is never NULL: a UTC time answers time_utc_loc. */
BURROW_BORROWS(ret) TimeLocation *time_location(Time t);

/* The zone in effect at t, its abbreviation such as "CET" and its offset in
 * seconds east of UTC. Go's Time.Zone. The name is borrowed from the
 * location. offset may be NULL. */
BURROW_BORROWS(ret) Str time_zone(Time t, Int *offset);

/* When the zone in effect at t started and when it ends. Either is the zero
 * Time when the zone runs off that end of the database. Go's Time.ZoneBounds.
 * end may be NULL. */
Time time_zone_bounds(Time t, Time *end);

/* Whether the zone in effect at t is daylight saving time. */
bool time_is_dst(Time t);

/* t as seconds, milliseconds, microseconds and nanoseconds since the unix
 * epoch. The nanosecond one is undefined, as in Go, for a time too far from
 * 1970 to fit, which is before 1678 or after 2262. */
int64_t time_unix(Time t);
int64_t time_unix_milli(Time t);
int64_t time_unix_micro(Time t);
int64_t time_unix_nano(Time t);

/* t rounded down, and rounded to the nearest, to a multiple of d since the
 * zero Time. Go's Time.Truncate and Time.Round. They work on the absolute
 * instant, so a d of an hour lands on the hour in a zone with a whole hour
 * offset and not in one with a half hour offset. A d of zero or less gives t
 * back without its monotonic reading. */
Time time_truncate(Time t, Duration d);
Time time_round(Time t, Duration d);

/* The binary encoding, which is Go's byte for byte: a version, the second and
 * nanosecond, and the zone offset in minutes, with a sixteenth byte in version
 * 2 for an offset that is not a whole minute. Go's Time.AppendBinary and
 * Time.MarshalBinary, and GobEncode, which is the same bytes.
 *
 * Only the offset survives, not the location's name: decoding gives UTC, Local
 * when the offset matches Local's at that instant, and a fixed zone otherwise,
 * which comes from a. A zone offset that does not fit is an error. */
BURROW_OWNS(ret) Slice time_append_binary(Time t, Alloc *a, Slice b, Error *err);
BURROW_OWNS(ret) Slice time_marshal_binary(Time t, Alloc *a, Error *err);
BURROW_STATIC(ret) Error time_unmarshal_binary(Time *t, Alloc *a, Slice data);
BURROW_OWNS(ret) Slice time_gob_encode(Time t, Alloc *a, Error *err);
BURROW_STATIC(ret) Error time_gob_decode(Time *t, Alloc *a, Slice data);

/* Layouts for time_format and time_parse, Go's constants of the same names.
 *
 * A layout is the reference time, Mon Jan 2 15:04:05 MST 2006, written the way
 * the text should look: 01/02 03:04:05PM '06 -0700 in numbers. "Jan" becomes
 * the month's short name, "2006" the four digit year, "15" the hour on a 24
 * hour clock, ".000" three digits of fraction, "-07:00" the zone offset and
 * "Z07:00" the same with Z for UTC. Anything that is not part of the reference
 * time is copied as it is. The whole list is in the time guide. */
#define TIME_LAYOUT BURROW_S("01/02 03:04:05PM '06 -0700")
#define TIME_ANSIC BURROW_S("Mon Jan _2 15:04:05 2006")
#define TIME_UNIX_DATE BURROW_S("Mon Jan _2 15:04:05 MST 2006")
#define TIME_RUBY_DATE BURROW_S("Mon Jan 02 15:04:05 -0700 2006")
#define TIME_RFC822 BURROW_S("02 Jan 06 15:04 MST")
#define TIME_RFC822_Z BURROW_S("02 Jan 06 15:04 -0700")
#define TIME_RFC850 BURROW_S("Monday, 02-Jan-06 15:04:05 MST")
#define TIME_RFC1123 BURROW_S("Mon, 02 Jan 2006 15:04:05 MST")
#define TIME_RFC1123_Z BURROW_S("Mon, 02 Jan 2006 15:04:05 -0700")
#define TIME_RFC3339 BURROW_S("2006-01-02T15:04:05Z07:00")
#define TIME_RFC3339_NANO BURROW_S("2006-01-02T15:04:05.999999999Z07:00")
#define TIME_KITCHEN BURROW_S("3:04PM")
#define TIME_STAMP BURROW_S("Jan _2 15:04:05")
#define TIME_STAMP_MILLI BURROW_S("Jan _2 15:04:05.000")
#define TIME_STAMP_MICRO BURROW_S("Jan _2 15:04:05.000000")
#define TIME_STAMP_NANO BURROW_S("Jan _2 15:04:05.000000000")
#define TIME_DATE_TIME BURROW_S("2006-01-02 15:04:05")
#define TIME_DATE_ONLY BURROW_S("2006-01-02")
#define TIME_TIME_ONLY BURROW_S("15:04:05")

/* t written out the way layout says, in t's location. Go's Time.Format.
 *
 *     Time t = time_date(2009, TIME_NOVEMBER, 10, 23, 0, 0, 0, time_utc_loc);
 *     Str s = time_format(t, a, TIME_RFC1123);
 *     // "Tue, 10 Nov 2009 23:00:00 UTC"
 */
BURROW_OWNS(ret) Str time_format(Time t, Alloc *a, Str layout);

/* time_format's text appended to b. Go's Time.AppendFormat. */
BURROW_OWNS(ret) Slice time_append_format(Time t, Alloc *a, Slice b, Str layout);

/* t as "2006-01-02 15:04:05.999999999 -0700 MST", and " m=+1.000000001" after
 * it when t has a monotonic reading. For people reading logs, not for parsing.
 * Go's Time.String. */
BURROW_OWNS(ret) Str time_string(Time t, Alloc *a);

/* t as the Go that would make it, such as time.Date(2009, time.November, 10,
 * 23, 0, 0, 0, time.UTC). Go's Time.GoString, which is what %#v prints. */
BURROW_OWNS(ret) Str time_go_string(Time t, Alloc *a);

/* Why time_parse gave up. Go's ParseError: the layout and the value, the
 * elements of each that did not match, and a message that replaces the usual
 * "cannot parse" sentence when there is one. errors_as with
 * TYPE_TIME_PARSE_ERROR finds it. */
typedef struct TimeParseError {
    Str layout;
    Str value;
    Str layout_elem;
    Str value_elem;
    Str message;
} TimeParseError;

extern const Type *const TYPE_TIME_PARSE_ERROR;

/* The error's text, from a. Go's ParseError.Error. */
BURROW_OWNS(ret) Str time_parse_error_error(Alloc *a, const TimeParseError *e);

/* e as an Error that owns copies of its strings, from a. */
BURROW_OWNS(ret) Error time_parse_error_as_error(Alloc *a, const TimeParseError *e);

/* The time that value, written the way layout says, stands for. Go's
 * time.Parse.
 *
 *     Error err;
 *     Time t = time_parse(a, TIME_RFC3339, BURROW_S("2006-01-02T15:04:05+07:00"), &err);
 *
 * Parts the layout leaves out are zero, or 1 for the month and day, and a time
 * with no zone in it is UTC. A zone offset or name that Local has at that
 * instant gives Local; any other gives a fixed zone made from a, which lives
 * as long as a does. An unknown name such as "XYZ" gets an offset of zero, as
 * in Go. The error is a TimeParseError. */
Time time_parse(Alloc *a, Str layout, Str value, Error *err);

/* time_parse with loc where time_parse uses UTC and Local: a time with no zone
 * is in loc, and an offset or name is matched against loc. Go's
 * time.ParseInLocation. */
Time time_parse_in_location(Alloc *a, Str layout, Str value, TimeLocation *loc,
                            Error *err);

/* The text encoding, RFC3339 with as many fraction digits as t needs. Go's
 * Time.AppendText and Time.MarshalText. A year outside 0 to 9999 or a zone
 * offset of 24 hours or more is an error, since RFC 3339 can not write them. */
BURROW_OWNS(ret) Slice time_append_text(Time t, Alloc *a, Slice b, Error *err);
BURROW_OWNS(ret) Slice time_marshal_text(Time t, Alloc *a, Error *err);

/* Reads what time_marshal_text writes, and any RFC3339 time. Go's
 * Time.UnmarshalText. *t is the zero Time on error. */
BURROW_STATIC(ret) Error time_unmarshal_text(Time *t, Alloc *a, Slice data);

/* The JSON encoding, the text encoding in double quotes. Go's
 * Time.MarshalJSON and Time.UnmarshalJSON, where null leaves *t alone. */
BURROW_OWNS(ret) Slice time_marshal_json(Time t, Alloc *a, Error *err);
BURROW_STATIC(ret) Error time_unmarshal_json(Time *t, Alloc *a, Slice data);

/* Time's descriptor, with the methods above, so fmt prints a Time with its
 * String and the encoders find its text, JSON, binary and gob forms. */
extern const Type burrow_type_Time;
#define TYPE_TIME TYPE_OF(Time)

/* Nanoseconds on a clock that only goes forwards, measured from an arbitrary
 * point that means nothing on its own.
 *
 *     int64_t start = burrow_nanotime();
 *     work();
 *     Duration took = burrow_nanotime() - start;
 *
 * It says burrow rather than time because Go has no such function. Go's
 * time.Now carries a monotonic reading around inside it and time.Since pulls it
 * back out, so a Go program never names the clock directly. time_now and
 * time_since do the same here, and this is the reading on its own, for code
 * that only wants an elapsed time and has no use for a date:
 * burrow/context.h measures a deadline on this clock, and so does the
 * runtime.
 *
 * Never goes backwards and never jumps, which is the point. The wall clock does
 * both whenever somebody sets the date or ntp corrects a drift, and a timeout
 * measured on it waits for an hour or fires twice.
 *
 * Callable from any thread, including one the runtime knows nothing about.
 * Costs a few nanoseconds everywhere, because every platform answers this out
 * of the vdso or its equivalent rather than from a system call.
 *
 * Inside a synctest bubble this is the bubble's clock and not the machine's, so
 * a deadline set in a bubble and a sleep in a bubble agree with each other.
 * Which also means the difference between two readings taken in a bubble is how
 * long the code under test thinks it took, not how long it really took, and the
 * second number is not available in there. See burrow/synctest.h.
 *
 * A bubble starts its clock at midnight UTC on 2000-01-01, which comes to
 * 946684800000000000 nanoseconds after the unix epoch. Go's number, and worth
 * knowing because it makes a test that prints an elapsed time print the same
 * thing on every machine and every run. */
int64_t burrow_nanotime(void);

/* Stops the calling goroutine for at least d.
 *
 * At least, and never exactly. The goroutine becomes runnable when the time is
 * up and then has to wait for a thread to pick it up, so a busy program hands it
 * back late. Every sleep in every language works this way. What burrow promises
 * is the same thing Go promises: not early, and not measured on a clock that
 * somebody can set backwards.
 *
 * A duration of zero or less returns straight away without giving up the
 * thread, which is Go's rule as well. It is not a yield, and if a yield is what
 * you want, that is runtime_gosched.
 *
 * Callable from a thread that is not one of the scheduler's, and then it sleeps
 * the thread rather than parking a goroutine, because there is no goroutine to
 * park. That is a courtesy for setup code and tests rather than a thing to build
 * on.
 *
 * Inside a synctest bubble this returns as soon as every other goroutine in the
 * bubble is blocked too, because that is when the bubble's clock jumps to the
 * next timer that is due. A sleep of an hour in there costs microseconds. The
 * order still holds: the goroutine that asked for a minute comes back before
 * the one that asked for an hour. See burrow/synctest.h. */
void time_sleep(Duration d);

/* A timer that runs a function once its time is up. Go's time.Timer.
 *
 * The name is what the mapping in docs/design/08-naming-abi.md gives for
 * time.Timer, package and type stuck together, the same rule that turns
 * strings.Builder into StringsBuilder. It stutters, and the alternative is a
 * mapping with exceptions in it, which costs more than the stutter does.
 *
 * Opaque, because the only thing the Go type has that you can reach is its
 * channel, and that is time_timer_c. One type covers both kinds Go has: the
 * one from time_after_func, which runs a function and has no channel, and the
 * one from time_new_timer, which sends on its channel. Stop, Reset and free
 * work on either. */
typedef struct TimeTimer TimeTimer;

/* Runs f in its own goroutine once d has gone by, and hands back the timer so
 * that it can be stopped or moved. Go's time.AfterFunc.
 *
 * In its own goroutine, which is Go's rule and matters more than it sounds. The
 * function runs on a fresh goroutine with a fresh stack, so it may block, take
 * locks, sleep again or talk to the network without holding up the thread that
 * noticed the timer was due. The cost is that there is no ordering between two
 * callbacks that come due at the same moment, exactly as in Go.
 *
 * The timer's memory comes from a, and it has to be given back with
 * time_timer_free. Go leaves that to the collector. This is the paired
 * constructor and destructor that docs/design/05-memory.md asks for whenever Go
 * itself has a Stop or a Close, and an arena user can ignore it because
 * arena_free already covers everything.
 *
 * NULL means the allocator or the timer heap would not give out memory, and
 * then nothing has been armed and nothing will run. A duration of zero or less
 * means the callback is due immediately and runs as soon as a thread looks.
 *
 * Has to be called from a goroutine, because the timer goes in the heap of the
 * P the caller is running on. */
BURROW_OWNS(ret) TimeTimer *time_after_func(Alloc *a, Duration d, Func f);

/* Stops a timer, and answers whether it was still waiting to run.
 *
 * False means the timer had already fired or had already been stopped. It does
 * not mean the callback has finished, and it does not mean the callback has even
 * started, because the callback runs on its own goroutine and a stop that loses
 * the race by a nanosecond still says false while the goroutine is still being
 * put together. Go has exactly this, and the answer there and here is the same:
 * a timer stop does not synchronise with the callback, so anything the callback
 * touches needs its own lock.
 *
 * Costs nothing but the timer's own lock. A stopped timer is left where it is
 * and marked, and the P that owns the heap throws it out the next time it walks
 * one, so a program that sets and clears a deadline on every read never touches
 * a heap at all. */
bool time_timer_stop(TimeTimer *t);

/* Arms a timer again for d from now, whether or not it was running, and answers
 * whether the call worked.
 *
 * `pending` may be NULL, and when it is not it is set to whether the timer was
 * still waiting to run, which is what Go's Reset returns. It is out here rather
 * than in the return value because arming a timer that had already fired can
 * need the P's heap to grow, and a heap that cannot grow is a failure a C
 * library has to report rather than panic on. burrow/timer.h says more about
 * that. False means the timer is not armed and will not run.
 *
 * Go's advice about Reset applies here unchanged: resetting a timer whose
 * callback is already running does not unrun it, and a program that needs to
 * know which of the two happened needs to say so itself, with a flag under a
 * lock the callback takes as well.
 *
 * On a timer from time_new_timer, Stop and Reset keep Go 1.23's promise: once
 * the call has returned, a receive on the channel only ever sees a value from
 * after it. A value that was already sitting in the channel is thrown away,
 * and a firing that was on its way is called off, and either one counts as the
 * timer having been pending. So the drain dance older Go code does,
 * `if !t.Stop() { <-t.C }`, is not needed, and it is harmless if it is there
 * with a non-blocking receive. */
bool time_timer_reset(TimeTimer *t, Duration d, bool *pending);

/* Hands the timer's memory back to the allocator it came from, and leaves the
 * TimeTimer * dangling, so it is the last thing you do with one.
 *
 * Stops the timer first, and takes it out of whatever heap it is sitting in, so
 * this is safe on a timer that never fired. That second part is the reason this
 * cannot be a plain mem_free: a timer that has been stopped is still in a P's
 * heap until that P gets round to throwing it out, and freeing the memory under
 * it would leave a pointer to a hole in the scheduler. Taking it out costs a
 * walk of the heap it is in, which is a few hundred nanoseconds on a P with a
 * thousand timers on it and nothing at all on one that has already fired.
 *
 * What it does not do is wait for a callback that is already running, for the
 * reason in time_timer_stop. A program that frees a timer whose callback is
 * still going is fine as far as the timer is concerned, since the callback has
 * its own goroutine and does not touch the timer, but it is on its own for
 * anything else that goroutine is holding.
 *
 * A timer from time_new_timer takes its channel with it, and no goroutine may
 * still be blocked on that channel, for the same reason as in chan_free.
 *
 * NULL is fine and does nothing. */
void time_timer_free(TimeTimer *t);

/* A timer that sends the time on its channel once d has gone by. Go's
 * time.NewTimer.
 *
 *     TimeTimer *t = time_new_timer(a, 2 * TIME_SECOND);
 *     Time fired;
 *     chan_recv(time_timer_c(t), &fired);
 *     time_timer_free(t);
 *
 * The channel carries Time values and is made from a along with the timer. It
 * reports a length and a capacity of zero, like Go 1.23's, though underneath it
 * has room for one value so that the timer never waits to send. A select with
 * a case on it is the usual way to put a deadline on something else.
 *
 * NULL means the allocator or the timer heap would not give out memory. Has to
 * be called from a goroutine, for the reason in time_after_func. */
BURROW_OWNS(ret) TimeTimer *time_new_timer(Alloc *a, Duration d);

/* The timer's channel, Go's t.C. NULL for a timer from time_after_func, which
 * has none, as in Go. The timer owns it, so it goes when the timer does. */
BURROW_BORROWS(ret, t) Chan *time_timer_c(const TimeTimer *t);

/* A channel that receives the time once d has gone by. Go's time.After, which
 * is time_new_timer with only the channel handed back. The name has chan in
 * it because time_after is Go's Time.After, the comparison.
 *
 * Go lets the collector have the timer once nobody can reach the channel.
 * There is no collector here, so the channel is the handle for both:
 * chan_free on it also stops the timer and frees it. Nothing needs doing with
 * an arena, which frees everything at once anyway. NULL means out of memory. */
BURROW_OWNS(ret) Chan *time_after_chan(Alloc *a, Duration d);

/* A ticker, which sends the time on its channel every d. Go's time.Ticker.
 *
 * Its own type because Go's is, with the same four things to do to it:
 * read the channel, stop it, reset it to a new period, and here, free it. */
typedef struct TimeTicker TimeTicker;

/* Starts a ticker with a period of d. Go's time.NewTicker.
 *
 * Ticks that nobody reads are dropped rather than queued, and a ticker that
 * falls behind skips the ticks it missed rather than sending them all at once
 * when it catches up. Each value is the time the tick was due, not the time it
 * was sent. All of that is Go's.
 *
 * A d of zero or less panics with Go's message, "non-positive interval for
 * NewTicker". NULL means out of memory. Has to be called from a goroutine. */
BURROW_OWNS(ret) TimeTicker *time_new_ticker(Alloc *a, Duration d);

/* The ticker's channel, Go's t.C. */
BURROW_BORROWS(ret, t) Chan *time_ticker_c(const TimeTicker *t);

/* Stops the ticker. No more ticks are sent, and one that was waiting in the
 * channel is thrown away. The channel is not closed, as in Go, so a goroutine
 * blocked on it stays blocked. NULL does nothing. */
void time_ticker_stop(TimeTicker *t);

/* Stops the ticker and starts it again with a period of d, with the first tick
 * d from now. Go's Ticker.Reset. Answers false only when the timer heap would
 * not grow, and then the ticker is stopped.
 *
 * A d of zero or less panics, "non-positive interval for Ticker.Reset", and so
 * does a NULL ticker, with Go's message for a Ticker that was never made. */
bool time_ticker_reset(TimeTicker *t, Duration d);

/* Stops the ticker and gives it and its channel back to the allocator. NULL is
 * fine and does nothing. */
void time_ticker_free(TimeTicker *t);

/* A channel that ticks every d, for the loop that runs until the program ends.
 * Go's time.Tick, which is time_new_ticker with only the channel handed back.
 *
 * NULL for a d of zero or less, which is Go's answer too, and for out of
 * memory. chan_free on the channel stops the ticker and frees it. */
BURROW_OWNS(ret) Chan *time_tick(Alloc *a, Duration d);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_TIME_H */
