/* The Time type: Go's time.go, from the wall and ext fields to the calendar.
 *
 * The representation is Go's exactly, because every other decision here leans
 * on it. wall has a flag in its top bit saying whether ext is a monotonic
 * reading. With the flag set, the 33 bits under it are the second since 1885,
 * which covers until 2157, and ext is the monotonic clock. Without it, those
 * bits are zero and ext is the second since year 1. The low 30 bits are the
 * nanosecond either way.
 *
 * The calendar is Go's too, which since Go 1.23 is Neri and Schneider's
 * Euclidean affine functions rather than the old 400 year cycles, counted in
 * days from March 1 of a year far enough back that every Time is after it.
 * Starting the year in March puts February's odd length at the end of it, and
 * then month and day come from a multiply and a shift.
 *
 * Go wraps on overflow and C does not, so every sum that Go lets wrap is done
 * here in uint64_t and converted back, which is what the TW_ macros are.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/time.h"

#include "burrow/atomic.h"
#include "burrow/clock.h"
#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/pal.h"
#include "burrow/panic.h"
#include "burrow/sched.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "time_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TW_ADD(a, b) ((int64_t)((uint64_t)(a) + (uint64_t)(b)))
#define TW_SUB(a, b) ((int64_t)((uint64_t)(a) - (uint64_t)(b)))
#define TW_MUL(a, b) ((int64_t)((uint64_t)(a) * (uint64_t)(b)))

#define TM_HAS_MONOTONIC ((uint64_t)1 << 63)
#define TM_MIN_WALL TIME_WALL_TO_INTERNAL
#define TM_MAX_WALL (TIME_WALL_TO_INTERNAL + (((int64_t)1 << 33) - 1))
#define TM_NSEC_MASK (((uint64_t)1 << 30) - 1)
#define TM_NSEC_SHIFT 30

#define TM_MIN_DURATION INT64_MIN
#define TM_MAX_DURATION INT64_MAX

/* ------------------------------------------------------ wall, ext and loc */

int32_t burrow__time_nsec(const Time *t) {
    return (int32_t)(t->wall & TM_NSEC_MASK);
}

int64_t burrow__time_sec(const Time *t) {
    if ((t->wall & TM_HAS_MONOTONIC) != 0)
        return TIME_WALL_TO_INTERNAL + (int64_t)(t->wall << 1 >> (TM_NSEC_SHIFT + 1));
    return t->ext;
}

int64_t burrow__time_unix_sec(const Time *t) {
    return TW_ADD(burrow__time_sec(t), TIME_INTERNAL_TO_UNIX);
}

static void tm_strip_mono(Time *t) {
    if ((t->wall & TM_HAS_MONOTONIC) != 0) {
        t->ext = burrow__time_sec(t);
        t->wall &= TM_NSEC_MASK;
    }
}

void burrow__time_add_sec(Time *t, int64_t d) {
    if ((t->wall & TM_HAS_MONOTONIC) != 0) {
        int64_t sec = (int64_t)(t->wall << 1 >> (TM_NSEC_SHIFT + 1));
        int64_t dsec = TW_ADD(sec, d);
        if (0 <= dsec && dsec <= ((int64_t)1 << 33) - 1) {
            t->wall = (t->wall & TM_NSEC_MASK) | (uint64_t)dsec << TM_NSEC_SHIFT |
                      TM_HAS_MONOTONIC;
            return;
        }
        /* Wall second no longer fits in the 33 bits, so fall back to a
         * Time without a monotonic reading. */
        tm_strip_mono(t);
    }
    int64_t sum = TW_ADD(t->ext, d);
    if ((sum > t->ext) == (d > 0))
        t->ext = sum;
    else if (d > 0)
        t->ext = INT64_MAX;
    else
        t->ext = -INT64_MAX;
}

void burrow__time_set_loc(Time *t, TimeLocation *loc) {
    if (loc == burrow__time_utc())
        loc = NULL;
    tm_strip_mono(t);
    t->loc = loc;
}

bool time_is_zero(Time t) {
    return t.wall == 0 && t.ext == 0;
}

bool time_after(Time t, Time u) {
    if ((t.wall & u.wall & TM_HAS_MONOTONIC) != 0)
        return t.ext > u.ext;
    int64_t ts = burrow__time_sec(&t);
    int64_t us = burrow__time_sec(&u);
    return ts > us || (ts == us && burrow__time_nsec(&t) > burrow__time_nsec(&u));
}

bool time_before(Time t, Time u) {
    if ((t.wall & u.wall & TM_HAS_MONOTONIC) != 0)
        return t.ext < u.ext;
    int64_t ts = burrow__time_sec(&t);
    int64_t us = burrow__time_sec(&u);
    return ts < us || (ts == us && burrow__time_nsec(&t) < burrow__time_nsec(&u));
}

Int time_compare(Time t, Time u) {
    int64_t tc;
    int64_t uc;
    if ((t.wall & u.wall & TM_HAS_MONOTONIC) != 0) {
        tc = t.ext;
        uc = u.ext;
    } else {
        tc = burrow__time_sec(&t);
        uc = burrow__time_sec(&u);
        if (tc == uc) {
            tc = burrow__time_nsec(&t);
            uc = burrow__time_nsec(&u);
        }
    }
    return tc < uc ? -1 : tc > uc ? 1 : 0;
}

bool time_equal(Time t, Time u) {
    if ((t.wall & u.wall & TM_HAS_MONOTONIC) != 0)
        return t.ext == u.ext;
    return burrow__time_sec(&t) == burrow__time_sec(&u) &&
           burrow__time_nsec(&t) == burrow__time_nsec(&u);
}

/* --------------------------------------------------------- Month, Weekday */

static const char *const tm_month_names[12] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December",
};

static const char *const tm_day_names[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};

/* "%!Month(" + the value as Go's fmtInt prints it, which is as a uint64, so
 * -1 is 18446744073709551615. */
static Str tm_bad_enum(const char *what, Int v, Alloc *a) {
    char buf[48];
    size_t n = strlen(what);
    memcpy(buf, what, n);
    char digits[24];
    int nd = 0;
    uint64_t u = (uint64_t)v;
    do {
        digits[nd++] = (char)('0' + u % 10);
        u /= 10;
    } while (u > 0);
    while (nd > 0)
        buf[n++] = digits[--nd];
    buf[n++] = ')';
    if (a == NULL)
        a = error_allocator();
    return str_clone(a, (Str){(const Byte *)buf, (Int)n});
}

static Str tm_cstr(const char *s) {
    return (Str){(const Byte *)s, (Int)strlen(s)};
}

Str time_month_string(TimeMonth m, Alloc *a) {
    if (TIME_JANUARY <= m && m <= TIME_DECEMBER)
        return tm_cstr(tm_month_names[m - 1]);
    return tm_bad_enum("%!Month(", m, a);
}

Str time_weekday_string(TimeWeekday d, Alloc *a) {
    if (TIME_SUNDAY <= d && d <= TIME_SATURDAY)
        return tm_cstr(tm_day_names[d]);
    return tm_bad_enum("%!Weekday(", d, a);
}

/* ---------------------------------------------------------------- calendar */

uint64_t burrow__time_date_to_abs_days(int64_t year, TimeMonth month, Int day) {
    uint32_t amonth = (uint32_t)month;
    uint32_t jan_feb = amonth < 3 ? 1 : 0;
    amonth += 12 * jan_feb;
    uint64_t y = (uint64_t)year - (uint64_t)jan_feb + (uint64_t)TIME_ABSOLUTE_YEARS;
    uint32_t ayday = (979 * amonth - 2919) >> 5;
    uint64_t century = y / 100;
    uint32_t cyear = (uint32_t)(y % 100);
    uint32_t cday = 1461 * cyear / 4;
    uint64_t centurydays = 146097 * century / 4;
    return centurydays + (uint64_t)TW_SUB(TW_ADD((int64_t)(cday + ayday), day), 1);
}

typedef struct TmSplit {
    uint64_t century;
    Int cyear;
    Int ayday;
} TmSplit;

static TmSplit tm_days_split(uint64_t days) {
    TmSplit s;
    uint64_t d = 4 * days + 3;
    s.century = d / 146097;
    uint32_t cd = (uint32_t)(d % 146097) | 3;
    uint64_t m = (uint64_t)2939745 * cd;
    s.cyear = (Int)(m >> 32);
    s.ayday = (Int)((uint32_t)m / 2939745 / 4);
    return s;
}

static void tm_ayday_split(Int ayday, Int *m, Int *mday) {
    uint32_t d = 2141 * (uint32_t)ayday + 197913;
    *m = (Int)(d >> 16);
    *mday = 1 + (Int)((d & 0xFFFF) / 2141);
}

static Int tm_jan_feb(Int ayday) {
    return ayday >= TIME_MARCH_THRU_DECEMBER ? 1 : 0;
}

static Int tm_leap(uint64_t century, Int cyear) {
    Int y4ok = cyear % 4 == 0 ? 1 : 0;
    Int y100ok = cyear != 0 ? 1 : 0;
    Int y400ok = century % 4 == 0 ? 1 : 0;
    return y4ok & (y100ok | y400ok);
}

static Int tm_year(uint64_t century, Int cyear, Int jan_feb) {
    return TW_ADD(
        TW_ADD((int64_t)(century * 100 - (uint64_t)TIME_ABSOLUTE_YEARS), cyear),
        jan_feb);
}

TimeYmd burrow__time_abs_date(uint64_t days) {
    TmSplit s = tm_days_split(days);
    Int amonth;
    TimeYmd r;
    tm_ayday_split(s.ayday, &amonth, &r.day);
    Int jf = tm_jan_feb(s.ayday);
    r.year = tm_year(s.century, s.cyear, jf);
    r.month = amonth - jf * 12;
    return r;
}

void burrow__time_abs_year_yday(uint64_t days, Int *year, Int *yday) {
    TmSplit s = tm_days_split(days);
    Int jf = tm_jan_feb(s.ayday);
    *year = tm_year(s.century, s.cyear, jf);
    *yday = s.ayday + (1 + 31 + 28) + (tm_leap(s.century, s.cyear) & ~jf) - 365 * jf;
}

static TimeWeekday tm_abs_weekday(uint64_t days) {
    return (TimeWeekday)((days + (uint64_t)TIME_WEDNESDAY) % 7);
}

uint64_t burrow__time_abs_sec(Time t) {
    TimeLocation *l = burrow__time_loc_get(t.loc);
    int64_t sec = burrow__time_unix_sec(&t);
    if (l != burrow__time_utc()) {
        if (l->cache_zone != NULL && l->cache_start <= sec && sec < l->cache_end)
            sec = TW_ADD(sec, l->cache_zone->offset);
        else
            sec = TW_ADD(sec, burrow__time_lookup(l, sec).offset);
    }
    return (uint64_t)sec + (uint64_t)TIME_UNIX_TO_ABSOLUTE;
}

static uint64_t tm_abs_days(Time t) {
    return burrow__time_abs_sec(t) / TZ_SECONDS_PER_DAY;
}

TimeDateRet time_date_of(Time t) {
    TimeYmd d = burrow__time_abs_date(tm_abs_days(t));
    return (TimeDateRet){d.year, d.month, d.day};
}

Int time_year(Time t) {
    TmSplit s = tm_days_split(tm_abs_days(t));
    return tm_year(s.century, s.cyear, tm_jan_feb(s.ayday));
}

TimeMonth time_month(Time t) {
    TmSplit s = tm_days_split(tm_abs_days(t));
    Int amonth;
    Int day;
    tm_ayday_split(s.ayday, &amonth, &day);
    return amonth - tm_jan_feb(s.ayday) * 12;
}

Int time_day(Time t) {
    TmSplit s = tm_days_split(tm_abs_days(t));
    Int amonth;
    Int day;
    tm_ayday_split(s.ayday, &amonth, &day);
    return day;
}

TimeWeekday time_weekday(Time t) {
    return tm_abs_weekday(tm_abs_days(t));
}

Int time_iso_week(Time t, Int *week) {
    /* The ISO week year and week are those of the Thursday in t's week, and
     * weeks start on Monday. */
    uint64_t days = tm_abs_days(t);
    uint64_t thu = days + (uint64_t)(TIME_THURSDAY - (tm_abs_weekday(days - 1) + 1));
    Int year;
    Int yday;
    burrow__time_abs_year_yday(thu, &year, &yday);
    BURROW_OUT(week, (yday - 1) / 7 + 1);
    return year;
}

static TimeClockRet tm_abs_clock(uint64_t abs) {
    TimeClockRet r;
    Int sec = (Int)(abs % TZ_SECONDS_PER_DAY);
    r.hour = sec / TZ_SECONDS_PER_HOUR;
    sec -= r.hour * TZ_SECONDS_PER_HOUR;
    r.min = sec / TZ_SECONDS_PER_MINUTE;
    r.sec = sec - r.min * TZ_SECONDS_PER_MINUTE;
    return r;
}

TimeClockRet time_clock(Time t) {
    return tm_abs_clock(burrow__time_abs_sec(t));
}

Int time_hour(Time t) {
    return (Int)(burrow__time_abs_sec(t) % TZ_SECONDS_PER_DAY) / TZ_SECONDS_PER_HOUR;
}

Int time_minute(Time t) {
    return (Int)(burrow__time_abs_sec(t) % TZ_SECONDS_PER_HOUR) / TZ_SECONDS_PER_MINUTE;
}

Int time_second(Time t) {
    return (Int)(burrow__time_abs_sec(t) % TZ_SECONDS_PER_MINUTE);
}

Int time_nanosecond(Time t) {
    return burrow__time_nsec(&t);
}

Int time_year_day(Time t) {
    Int year;
    Int yday;
    burrow__time_abs_year_yday(tm_abs_days(t), &year, &yday);
    return yday;
}

bool burrow__time_is_leap(Int year) {
    Int mask = year % 25 != 0 ? 3 : 0xf;
    return (year & mask) == 0;
}

Int burrow__time_days_before(TimeMonth m) {
    Int adj = m >= TIME_MARCH ? -2 : 0;
    return (214 * m - 211) / 7 + adj;
}

Int burrow__time_days_in(TimeMonth m, Int year) {
    if (m == TIME_FEBRUARY)
        return burrow__time_is_leap(year) ? 29 : 28;
    return 30 + ((m + (m >> 3)) & 1);
}

/* ---------------------------------------------------------- arithmetic */

Time time_add(Time t, Duration d) {
    int64_t dsec = d / 1000000000;
    int32_t nsec = burrow__time_nsec(&t) + (int32_t)(d % 1000000000);
    if (nsec >= 1000000000) {
        dsec++;
        nsec -= 1000000000;
    } else if (nsec < 0) {
        dsec--;
        nsec += 1000000000;
    }
    t.wall = (t.wall & ~TM_NSEC_MASK) | (uint64_t)nsec;
    burrow__time_add_sec(&t, dsec);
    if ((t.wall & TM_HAS_MONOTONIC) != 0) {
        int64_t te = TW_ADD(t.ext, d);
        if ((d < 0 && te > t.ext) || (d > 0 && te < t.ext))
            tm_strip_mono(&t); /* the monotonic reading ran off its end */
        else
            t.ext = te;
    }
    return t;
}

static Duration tm_sub_mono(int64_t t, int64_t u) {
    Duration d = TW_SUB(t, u);
    if (d < 0 && t > u)
        return TM_MAX_DURATION;
    if (d > 0 && t < u)
        return TM_MIN_DURATION;
    return d;
}

Duration time_sub(Time t, Time u) {
    if ((t.wall & u.wall & TM_HAS_MONOTONIC) != 0)
        return tm_sub_mono(t.ext, u.ext);
    Duration d =
        TW_ADD(TW_MUL(TW_SUB(burrow__time_sec(&t), burrow__time_sec(&u)), TIME_SECOND),
               burrow__time_nsec(&t) - burrow__time_nsec(&u));
    if (time_equal(time_add(u, d), t))
        return d;
    if (time_before(t, u))
        return TM_MIN_DURATION;
    return TM_MAX_DURATION;
}

/* ------------------------------------------------------------------ Now */

/* Go's startNano, the monotonic reading a Time's own reading is measured
 * from, so that a Time printed with its m=+1.5 says how far into the run it
 * was taken. Go sets it when the package starts. Here that is the first
 * time_now, one nanosecond before the reading it takes, which gives the same
 * m=+0.000000001 Go gives a Now taken at the very start. */
static uint64_t tm_start_nano;

static int64_t tm_start(int64_t now) {
    uint64_t s = burrow__atomic_load_acquire_u64(&tm_start_nano);
    if (s != 0)
        return (int64_t)s;
    uint64_t want = (uint64_t)now - 1;
    if (burrow__atomic_cas_u64(&tm_start_nano, &s, want))
        return (int64_t)want;
    return (int64_t)s;
}

/* Go's runtimeNow: the wall clock and the monotonic clock, or inside a bubble
 * the bubble's clock for both. mono of zero means there is no monotonic
 * reading to carry. */
static void tm_runtime_now(int64_t *sec, int32_t *nsec, int64_t *mono) {
    burrow__Bubble *b = burrow__curbubble();
    int64_t wall;
    if (b != NULL) {
        wall = burrow__bubble_now(b);
        *mono = wall;
    } else {
        wall = pal_clock_realtime();
        *mono = burrow__nanotime();
    }
    *sec = wall / 1000000000;
    *nsec = (int32_t)(wall % 1000000000);
    if (*nsec < 0) {
        *nsec += 1000000000;
        *sec -= 1;
    }
}

Time time_now(void) {
    int64_t sec;
    int32_t nsec;
    int64_t mono;
    tm_runtime_now(&sec, &nsec, &mono);
    if (mono == 0)
        return (Time){(uint64_t)nsec, sec + TIME_UNIX_TO_INTERNAL, time_local_loc};
    mono -= tm_start(mono);
    sec += TIME_UNIX_TO_INTERNAL - TM_MIN_WALL;
    if ((uint64_t)sec >> 33 != 0)
        /* The wall second does not fit, which is before 1885 or after
         * 2157, so the reading goes. */
        return (Time){(uint64_t)nsec, sec + TM_MIN_WALL, time_local_loc};
    return (Time){TM_HAS_MONOTONIC | (uint64_t)sec << TM_NSEC_SHIFT | (uint64_t)nsec,
                  mono, time_local_loc};
}

Duration time_since(Time t) {
    if ((t.wall & TM_HAS_MONOTONIC) != 0 && burrow__curbubble() == NULL) {
        int64_t now = burrow__nanotime();
        return tm_sub_mono(now - tm_start(now), t.ext);
    }
    return time_sub(time_now(), t);
}

Duration time_until(Time t) {
    if ((t.wall & TM_HAS_MONOTONIC) != 0 && burrow__curbubble() == NULL) {
        int64_t now = burrow__nanotime();
        return tm_sub_mono(t.ext, now - tm_start(now));
    }
    return time_sub(t, time_now());
}

Time burrow__time_unix_time(int64_t sec, int32_t nsec) {
    return (Time){(uint64_t)nsec, TW_ADD(sec, TIME_UNIX_TO_INTERNAL), time_local_loc};
}

Time time_from_unix(int64_t sec, int64_t nsec) {
    if (nsec < 0 || nsec >= 1000000000) {
        int64_t n = nsec / 1000000000;
        sec = TW_ADD(sec, n);
        nsec -= n * 1000000000;
        if (nsec < 0) {
            nsec += 1000000000;
            sec = TW_SUB(sec, 1);
        }
    }
    return burrow__time_unix_time(sec, (int32_t)nsec);
}

Time time_from_unix_milli(int64_t msec) {
    return time_from_unix(msec / 1000, (msec % 1000) * 1000000);
}

Time time_from_unix_micro(int64_t usec) {
    return time_from_unix(usec / 1000000, (usec % 1000000) * 1000);
}

/* ------------------------------------------------------------- locations */

Time time_utc(Time t) {
    burrow__time_set_loc(&t, burrow__time_utc());
    return t;
}

Time time_local(Time t) {
    burrow__time_set_loc(&t, time_local_loc);
    return t;
}

Time time_in(Time t, TimeLocation *loc) {
    if (loc == NULL)
        panic_str(BURROW_S("time: missing Location in call to Time.In"));
    burrow__time_set_loc(&t, loc);
    return t;
}

TimeLocation *time_location(Time t) {
    return t.loc != NULL ? t.loc : time_utc_loc;
}

Str time_zone(Time t, Int *offset) {
    TzLookup z = burrow__time_lookup(t.loc, burrow__time_unix_sec(&t));
    BURROW_OUT(offset, z.offset);
    return z.name;
}

Time time_zone_bounds(Time t, Time *end) {
    TzLookup z = burrow__time_lookup(t.loc, burrow__time_unix_sec(&t));
    Time start = {0};
    Time e = {0};
    if (z.start != TZ_ALPHA) {
        start = burrow__time_unix_time(z.start, 0);
        burrow__time_set_loc(&start, t.loc);
    }
    if (z.end != TZ_OMEGA) {
        e = burrow__time_unix_time(z.end, 0);
        burrow__time_set_loc(&e, t.loc);
    }
    BURROW_OUT(end, e);
    return start;
}

bool time_is_dst(Time t) {
    return burrow__time_lookup(t.loc, time_unix(t)).is_dst;
}

int64_t time_unix(Time t) {
    return burrow__time_unix_sec(&t);
}

int64_t time_unix_milli(Time t) {
    return TW_ADD(TW_MUL(burrow__time_unix_sec(&t), 1000),
                  burrow__time_nsec(&t) / 1000000);
}

int64_t time_unix_micro(Time t) {
    return TW_ADD(TW_MUL(burrow__time_unix_sec(&t), 1000000),
                  burrow__time_nsec(&t) / 1000);
}

int64_t time_unix_nano(Time t) {
    return TW_ADD(TW_MUL(burrow__time_unix_sec(&t), 1000000000), burrow__time_nsec(&t));
}

/* ------------------------------------------------------------------ Date */

/* Go's norm: hi and lo such that lo is in [0, base), carrying the rest into
 * hi. */
static void tm_norm(Int *hi, Int *lo, Int base) {
    if (*lo < 0) {
        /* -lo-1 written so that it does not overflow for the smallest lo. */
        Int n = (-(*lo + 1)) / base + 1;
        *hi = TW_SUB(*hi, n);
        *lo = TW_ADD(*lo, TW_MUL(n, base));
    }
    if (*lo >= base) {
        Int n = *lo / base;
        *hi = TW_ADD(*hi, n);
        *lo -= n * base;
    }
}

Time time_date(Int year, TimeMonth month, Int day, Int hour, Int min, Int sec, Int nsec,
               TimeLocation *loc) {
    if (loc == NULL)
        panic_str(BURROW_S("time: missing Location in call to Date"));

    /* Normalise the month and the clock, carrying into the day. */
    Int m = TW_SUB(month, 1);
    tm_norm(&year, &m, 12);
    month = m + 1;

    tm_norm(&sec, &nsec, 1000000000);
    tm_norm(&min, &sec, 60);
    tm_norm(&hour, &min, 60);
    tm_norm(&day, &hour, 24);

    int64_t unix = TW_ADD(TW_ADD(TW_MUL(burrow__time_date_to_abs_days(year, month, day),
                                        TZ_SECONDS_PER_DAY),
                                 TW_ADD(TW_ADD(TW_MUL(hour, TZ_SECONDS_PER_HOUR),
                                               TW_MUL(min, TZ_SECONDS_PER_MINUTE)),
                                        sec)),
                          TIME_ABSOLUTE_TO_UNIX);

    /* Look for the zone offset for the expected time, then adjust if the
     * guess was on the wrong side of a transition. */
    TzLookup z = burrow__time_lookup(loc, unix);
    if (z.offset != 0) {
        int64_t utc = TW_SUB(unix, z.offset);
        if (utc < z.start || utc >= z.end)
            z = burrow__time_lookup(loc, utc);
        unix = TW_SUB(unix, z.offset);
    }

    Time t = burrow__time_unix_time(unix, (int32_t)nsec);
    burrow__time_set_loc(&t, loc);
    return t;
}

Time time_add_date(Time t, Int years, Int months, Int days) {
    TimeDateRet d = time_date_of(t);
    TimeClockRet c = time_clock(t);
    return time_date(TW_ADD(d.year, years), TW_ADD(d.month, months),
                     TW_ADD(d.day, days), c.hour, c.min, c.sec, burrow__time_nsec(&t),
                     time_location(t));
}

/* --------------------------------------------------- Truncate and Round */

static bool tm_less_than_half(Duration x, Duration y) {
    return (uint64_t)x + (uint64_t)x < (uint64_t)y;
}

/* Go's div: t mod d, and whether t/d is odd, over the whole 94 bit range of
 * a Time, which is why the general case is a long division by hand. */
static Duration tm_div(Time t, Duration d, Int *qmod2) {
    bool neg = false;
    int32_t nsec = burrow__time_nsec(&t);
    int64_t sec = burrow__time_sec(&t);
    Duration r;
    Int q;
    if (sec < 0) {
        /* Work on the absolute value and fix up at the end. */
        neg = true;
        sec = TW_SUB(0, sec);
        nsec = -nsec;
        if (nsec < 0) {
            nsec += 1000000000;
            sec--;
        }
    }

    if (d < TIME_SECOND && TIME_SECOND % (d + d) == 0) {
        /* d divides a second, so only the nanoseconds matter. */
        q = (Int)(nsec / (int32_t)d) & 1;
        r = nsec % (int32_t)d;
    } else if (d % TIME_SECOND == 0) {
        /* d is whole seconds, so the nanoseconds come along unchanged. */
        int64_t d1 = d / TIME_SECOND;
        q = (Int)(sec / d1) & 1;
        r = (sec % d1) * TIME_SECOND + nsec;
    } else {
        /* Compute nanoseconds as a 128 bit number. */
        uint64_t usec = (uint64_t)sec;
        uint64_t tmp = (usec >> 32) * 1000000000;
        uint64_t u1 = tmp >> 32;
        uint64_t u0 = tmp << 32;
        tmp = (usec & 0xFFFFFFFF) * 1000000000;
        uint64_t u0x = u0;
        u0 = u0 + tmp;
        if (u0 < u0x)
            u1++;
        u0x = u0;
        u0 = u0 + (uint64_t)nsec;
        if (u0 < u0x)
            u1++;

        /* Compute the remainder by subtracting r<<k for decreasing k. */
        uint64_t d1 = (uint64_t)d;
        while (d1 >> 63 != 1)
            d1 <<= 1;
        uint64_t d0 = 0;
        for (;;) {
            q = 0;
            if (u1 > d1 || (u1 == d1 && u0 >= d0)) {
                q = 1;
                u0x = u0;
                u0 = u0 - d0;
                if (u0 > u0x)
                    u1--;
                u1 -= d1;
            }
            if (d1 == 0 && d0 == (uint64_t)d)
                break;
            d0 >>= 1;
            d0 |= (d1 & 1) << 63;
            d1 >>= 1;
        }
        r = (Duration)u0;
    }

    if (neg && r != 0) {
        /* The absolute value was rounded down, so the real one goes the
         * other way. */
        q ^= 1;
        r = d - r;
    }
    *qmod2 = q;
    return r;
}

Time time_truncate(Time t, Duration d) {
    tm_strip_mono(&t);
    if (d <= 0)
        return t;
    Int q;
    Duration r = tm_div(t, d, &q);
    return time_add(t, -r);
}

Time time_round(Time t, Duration d) {
    tm_strip_mono(&t);
    if (d <= 0)
        return t;
    Int q;
    Duration r = tm_div(t, d, &q);
    if (tm_less_than_half(r, d))
        return time_add(t, -r);
    return time_add(t, d - r);
}

/* ---------------------------------------------------------------- binary */

enum { TM_BINARY_V1 = 1, TM_BINARY_V2 = 2 };

static Error tm_err(const char *s) {
    return errors_new(error_allocator(), tm_cstr(s));
}

Slice time_append_binary(Time t, Alloc *a, Slice b, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    int16_t offset_min;
    int8_t offset_sec = 0;
    Byte version = TM_BINARY_V1;

    if (time_location(t) == time_utc_loc) {
        offset_min = -1;
    } else {
        Int offset;
        (void)time_zone(t, &offset);
        if (offset % 60 != 0) {
            version = TM_BINARY_V2;
            offset_sec = (int8_t)(offset % 60);
        }
        offset /= 60;
        if (offset < -32768 || offset == -1 || offset > 32767) {
            BURROW_OUT(err, tm_err("Time.MarshalBinary: unexpected zone offset"));
            return b;
        }
        offset_min = (int16_t)offset;
    }

    int64_t sec = burrow__time_sec(&t);
    int32_t nsec = burrow__time_nsec(&t);
    Byte enc[16];
    enc[0] = version;
    for (int i = 0; i < 8; i++)
        enc[1 + i] = (Byte)((uint64_t)sec >> (56 - 8 * i));
    for (int i = 0; i < 4; i++)
        enc[9 + i] = (Byte)((uint32_t)nsec >> (24 - 8 * i));
    enc[13] = (Byte)((uint16_t)offset_min >> 8);
    enc[14] = (Byte)offset_min;
    Int n = 15;
    if (version == TM_BINARY_V2)
        enc[n++] = (Byte)offset_sec;
    if (b.elem == NULL)
        b = slice_nil(TYPE_BYTE);
    Slice out = slice_append(a, b, enc, n);
    if (out.len != b.len + n) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return b;
    }
    return out;
}

Slice time_marshal_binary(Time t, Alloc *a, Error *err) {
    Error e = BURROW_NO_ERROR;
    Slice b = slice_make(a, TYPE_BYTE, 0, 16);
    b = time_append_binary(t, a, b, &e);
    BURROW_OUT(err, e);
    if (BURROW_FAILED(e)) {
        if (b.p != NULL)
            mem_free(a, b.p, (size_t)b.cap, 1);
        return slice_nil(TYPE_BYTE);
    }
    return b;
}

Error time_unmarshal_binary(Time *t, Alloc *a, Slice data) {
    const Byte *buf = data.p;
    if (data.len == 0)
        return tm_err("Time.UnmarshalBinary: no data");
    Byte version = buf[0];
    if (version != TM_BINARY_V1 && version != TM_BINARY_V2)
        return tm_err("Time.UnmarshalBinary: unsupported version");
    Int want = 1 + 8 + 4 + 2;
    if (version == TM_BINARY_V2)
        want++;
    if (data.len != want)
        return tm_err("Time.UnmarshalBinary: invalid length");

    buf++;
    uint64_t sec = 0;
    for (int i = 0; i < 8; i++)
        sec = sec << 8 | buf[i];
    buf += 8;
    uint32_t nsec = 0;
    for (int i = 0; i < 4; i++)
        nsec = nsec << 8 | buf[i];
    buf += 4;
    Int offset = (Int)(int16_t)(uint16_t)((uint16_t)buf[0] << 8 | buf[1]) * 60;
    if (version == TM_BINARY_V2)
        offset += (int8_t)buf[2];

    Time r = {0};
    /* Go puts the nanosecond in wall as it came, sign and all. */
    r.wall = (uint64_t)(int64_t)(int32_t)nsec;
    r.ext = (int64_t)sec;

    if (offset == -60) { /* Go writes -1 * 60, the offset that means UTC */
        burrow__time_set_loc(&r, burrow__time_utc());
    } else if (burrow__time_lookup(time_local_loc, burrow__time_unix_sec(&r)).offset ==
               offset) {
        burrow__time_set_loc(&r, time_local_loc);
    } else {
        TimeLocation *l = burrow__time_fixed_zone(a, BURROW_S(""), offset);
        if (l == NULL)
            return burrow_err_out_of_memory;
        burrow__time_set_loc(&r, l);
    }
    *t = r;
    return BURROW_NO_ERROR;
}

Slice time_gob_encode(Time t, Alloc *a, Error *err) {
    return time_marshal_binary(t, a, err);
}

Error time_gob_decode(Time *t, Alloc *a, Slice data) {
    return time_unmarshal_binary(t, a, data);
}

/* ------------------------------------------------------------- descriptors */

static Str tm_month_m_string(TimeMonth *self) {
    return time_month_string(*self, error_allocator());
}

static Str tm_weekday_m_string(TimeWeekday *self) {
    return time_weekday_string(*self, error_allocator());
}

#define TM_SIG_STRING(IN, OUT) OUT(Str)
#define TM_MONTH_METHODS(M, T) M(T, String, tm_month_m_string, TM_SIG_STRING)
#define TM_WEEKDAY_METHODS(M, T) M(T, String, tm_weekday_m_string, TM_SIG_STRING)

BURROW_METHODS_DEFINE(TimeMonth, TM_MONTH_METHODS);
BURROW_METHODS_DEFINE(TimeWeekday, TM_WEEKDAY_METHODS);

const Type burrow_type_TimeMonth = {
    {(const Byte *)"Month", 5},
    {(const Byte *)"time", 4},
    KIND_INT,
    (uint32_t)sizeof(TimeMonth),
    (uint16_t)_Alignof(TimeMonth),
    0,
    (uint16_t)(sizeof(burrow__methods_TimeMonth) /
               sizeof(burrow__methods_TimeMonth[0])),
    NULL,
    burrow__methods_TimeMonth,
    NULL,
    NULL,
    0,
    0x746d6f6eU, /* "tmon" */
    NULL,
};

const Type burrow_type_TimeWeekday = {
    {(const Byte *)"Weekday", 7},
    {(const Byte *)"time", 4},
    KIND_INT,
    (uint32_t)sizeof(TimeWeekday),
    (uint16_t)_Alignof(TimeWeekday),
    0,
    (uint16_t)(sizeof(burrow__methods_TimeWeekday) /
               sizeof(burrow__methods_TimeWeekday[0])),
    NULL,
    burrow__methods_TimeWeekday,
    NULL,
    NULL,
    0,
    0x74776b64U, /* "twkd" */
    NULL,
};
