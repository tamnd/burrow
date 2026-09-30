/* tests/time_calendar_test_gen.h, from tools/gen-time-tests.sh, holds what
 * Go's package time says about a list of instants in a list of zones: the
 * date, the clock, the zone, its bounds and the binary encoding of each. The
 * zones are TZif files from Go's own zoneinfo.zip, carried in the header as
 * bytes, so nothing here depends on the machine's zoneinfo. It also holds
 * Date, AddDate, Truncate, Round, Add and Sub on their awkward inputs, the
 * Duration methods, Go's tzset tables and every cut and flipped byte of two
 * TZif files. The tests here work each case out again and compare.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/time/time_internal.h"

#include "burrow/error.h"
#include "burrow/mem/heap.h"
#include "burrow/slice.h"
#include "burrow/time.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct TzBlob {
    const char *name;
    const char *data;
    long long len;
} TzBlob;

typedef struct TzFixed {
    const char *name;
    long long off;
} TzFixed;

typedef struct TCase {
    long long loc;
    int64_t sec;
    long long nsec;
    int64_t year;
    long long month, day, hour, min, secs, weekday, yday;
    int64_t iso_year;
    long long iso_week;
    const char *zone;
    long long offset;
    long long is_dst;
    int64_t bstart;
    long long bstart_zero;
    int64_t bend;
    long long bend_zero;
    const char *bin;
    const char *bin_err;
    int64_t milli;
    long long bin_len;
    int64_t micro;
    int64_t nano;
} TCase;

typedef struct DCase {
    int loc;
    long long year, month, day, hour, min, sec, nsec;
    int64_t unix;
    long long got_nsec;
    const char *zone;
    long long offset;
} DCase;

typedef struct ACase {
    int loc;
    int64_t base;
    long long years, months, days;
    int64_t unix;
    long long nsec;
} ACase;

typedef struct RCase {
    int64_t sec;
    long long nsec;
    int64_t d;
    int64_t trunc_sec;
    long long trunc_nsec;
    int64_t round_sec;
    long long round_nsec;
} RCase;

typedef struct SCase {
    int64_t a, an, b, bn;
    int64_t sub;
    int cmp;
} SCase;

typedef struct AddCase {
    int64_t sec, nsec, d, unix;
    long long got_nsec;
} AddCase;

typedef struct DurCase {
    int64_t d, ns, us, ms;
    double s, m, h;
    int64_t abs;
} DurCase;

typedef struct DurRound {
    int64_t d, m, trunc, round;
} DurRound;

typedef struct NameCase {
    int64_t v;
    const char *want;
} NameCase;

typedef struct BinCase {
    const char *data;
    int len;
    const char *err;
    int64_t unix;
    long long nsec;
    const char *zone;
    long long offset;
} BinCase;

typedef struct TzsetCase {
    const char *s;
    int64_t last, sec;
    long long ok;
    const char *name;
    long long off;
    int64_t start, end;
    long long is_dst;
} TzsetCase;

typedef struct TzsetNameCase {
    const char *s;
    int ok;
    const char *name, *rest;
} TzsetNameCase;

typedef struct TzsetOffsetCase {
    const char *s;
    int ok;
    long long off;
    const char *rest;
} TzsetOffsetCase;

typedef struct TzsetRuleCase {
    const char *s;
    int ok;
    int kind;
    long long day, week, mon, time;
    const char *rest;
} TzsetRuleCase;

typedef struct BadCase {
    int zone;
    int cut;
    int pos;
    int x;
    const char *err;
    const char *zname[3];
    long long zoff[3];
} BadCase;

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
#include "time_calendar_test_gen.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define LEN(x) (sizeof(x) / sizeof((x)[0]))
#define NZONES ((int)LEN(g_zones))
#define NLOCS (NZONES + (int)LEN(g_fixed))

static Str cstr(const char *s) {
    return (Str){(const Byte *)s, s == NULL ? 0 : (Int)strlen(s)};
}

static Str err_str(Error err) {
    return BURROW_FAILED(err) ? error_text(err) : cstr("<nil>");
}

static Slice blob(const char *p, long long n) {
    return slice_from((void *)(uintptr_t)p, (Int)n, (Int)n, TYPE_BYTE);
}

/* The locations the cases are in, by the number the generator gave them:
 * -1 for UTC, then the zones, then the fixed zones. */
static TimeLocation *g_locs[64];

static void load_locs(TestingT *t) {
    for (int i = 0; i < NZONES; i++) {
        if (g_locs[i] != NULL)
            continue;
        Error err = BURROW_NO_ERROR;
        g_locs[i] = time_load_location_from_tz_data(
            heap_allocator(), cstr(g_zones[i].name),
            blob(g_zones[i].data, g_zones[i].len), &err);
        if (g_locs[i] == NULL)
            testing_t_fatalf_v(t, "%s: %s", g_zones[i].name, err_str(err));
    }
    for (int i = 0; i < (int)LEN(g_fixed); i++) {
        if (g_locs[NZONES + i] == NULL)
            g_locs[NZONES + i] = time_fixed_zone(heap_allocator(),
                                                 cstr(g_fixed[i].name), g_fixed[i].off);
    }
}

static TimeLocation *loc_of(long long i) {
    return i < 0 ? time_utc_loc : g_locs[i];
}

static void TestLocationNames(TestingT *t) {
    load_locs(t);
    CHECK(str_eq(time_location_string(time_utc_loc), cstr("UTC")));
    CHECK(str_eq(time_location_string(NULL), cstr("UTC")));
    CHECK(time_location_string(time_local_loc).len > 0);
    for (int i = 0; i < NZONES; i++)
        CHECK(str_eq(time_location_string(g_locs[i]), cstr(g_zones[i].name)));
    for (int i = 0; i < (int)LEN(g_fixed); i++)
        CHECK(str_eq(time_location_string(g_locs[NZONES + i]), cstr(g_fixed[i].name)));
}

static void TestTimes(TestingT *t) {
    load_locs(t);
    for (size_t i = 0; i < LEN(g_times); i++) {
        const TCase *c = &g_times[i];
        TimeLocation *loc = loc_of(c->loc);
        Str lname = time_location_string(loc);
        Time tm = time_in(time_from_unix(c->sec, c->nsec), loc);
        TimeDateRet d = time_date_of(tm);
        TimeClockRet k = time_clock(tm);
        if (d.year != c->year || d.month != c->month || d.day != c->day ||
            k.hour != c->hour || k.min != c->min || k.sec != c->secs ||
            time_nanosecond(tm) != c->nsec)
            testing_t_errorf_v(
                t, "%s %d.%d: got %d-%d-%d %d:%d:%d.%d, want %d-%d-%d %d:%d:%d", lname,
                c->sec, c->nsec, d.year, d.month, d.day, k.hour, k.min, k.sec,
                time_nanosecond(tm), c->year, c->month, c->day, c->hour, c->min,
                c->secs);
        if (time_year(tm) != c->year || time_month(tm) != c->month ||
            time_day(tm) != c->day || time_hour(tm) != c->hour ||
            time_minute(tm) != c->min || time_second(tm) != c->secs)
            testing_t_errorf_v(t, "%s %d: accessors disagree with Date and Clock",
                               lname, c->sec);
        if (time_weekday(tm) != c->weekday || time_year_day(tm) != c->yday)
            testing_t_errorf_v(t, "%s %d: weekday %d yday %d, want %d %d", lname,
                               c->sec, time_weekday(tm), time_year_day(tm), c->weekday,
                               c->yday);
        Int week = 0;
        Int iy = time_iso_week(tm, &week);
        if (iy != c->iso_year || week != c->iso_week)
            testing_t_errorf_v(t, "%s %d: ISOWeek = %d %d, want %d %d", lname, c->sec,
                               iy, week, c->iso_year, c->iso_week);
        Int off = 0;
        Str zn = time_zone(tm, &off);
        if (!str_eq(zn, cstr(c->zone)) || off != c->offset ||
            time_is_dst(tm) != (c->is_dst != 0))
            testing_t_errorf_v(t, "%s %d: Zone = %q %d %v, want %q %d %v", lname,
                               c->sec, zn, off, time_is_dst(tm), c->zone, c->offset,
                               c->is_dst != 0);
        Time end;
        Time start = time_zone_bounds(tm, &end);
        if (time_is_zero(start) != (c->bstart_zero != 0) ||
            time_is_zero(end) != (c->bend_zero != 0) || time_unix(start) != c->bstart ||
            time_unix(end) != c->bend)
            testing_t_errorf_v(t, "%s %d: ZoneBounds = %d %d, want %d %d", lname,
                               c->sec, time_unix(start), time_unix(end), c->bstart,
                               c->bend);
        if (time_unix(tm) != c->sec || time_unix_milli(tm) != c->milli ||
            time_unix_micro(tm) != c->micro || time_unix_nano(tm) != c->nano)
            testing_t_errorf_v(t, "%s %d: unix getters %d %d %d %d, want %d %d %d %d",
                               lname, c->sec, time_unix(tm), time_unix_milli(tm),
                               time_unix_micro(tm), time_unix_nano(tm), c->sec,
                               c->milli, c->micro, c->nano);

        /* The date and clock back through time_date land on the same
         * instant, except where they name a time that happens twice. */
        Time back = time_date(d.year, d.month, d.day, k.hour, k.min, k.sec,
                              time_nanosecond(tm), loc);
        Int boff = 0;
        (void)time_zone(back, &boff);
        if (boff == off && !time_equal(back, tm))
            testing_t_errorf_v(t, "%s %d: Date round trip gave %d", lname, c->sec,
                               time_unix(back));

        Error err = BURROW_NO_ERROR;
        Slice b = time_marshal_binary(tm, heap_allocator(), &err);
        Str want_err = cstr(c->bin_err == NULL ? "<nil>" : c->bin_err);
        if (!str_eq(err_str(err), want_err)) {
            testing_t_errorf_v(t, "%s %d: MarshalBinary error %q, want %q", lname,
                               c->sec, err_str(err), want_err);
        } else if (c->bin_err == NULL) {
            Str got = {b.p, b.len};
            Str wantb = {(const Byte *)c->bin, c->bin_len};
            if (!str_eq(got, wantb))
                testing_t_errorf_v(t, "%s %d: MarshalBinary = %x, want %x", lname,
                                   c->sec, got, wantb);
            Time u = {0, 0, NULL};
            Error e2 = time_unmarshal_binary(&u, heap_allocator(), b);
            Int uoff = 0;
            (void)time_zone(u, &uoff);
            if (BURROW_FAILED(e2) || !time_equal(u, tm) || uoff != off)
                testing_t_errorf_v(t, "%s %d: UnmarshalBinary gave %d offset %d, %v",
                                   lname, c->sec, time_unix(u), uoff, err_str(e2));
            time_location_free(time_location(u));
        }
        if (b.p != NULL)
            mem_free(heap_allocator(), b.p, (size_t)b.cap, 1);
    }
}

static void TestDate(TestingT *t) {
    load_locs(t);
    for (size_t i = 0; i < LEN(g_dates); i++) {
        const DCase *c = &g_dates[i];
        Time tm = time_date(c->year, c->month, c->day, c->hour, c->min, c->sec, c->nsec,
                            loc_of(c->loc));
        Int off = 0;
        Str zn = time_zone(tm, &off);
        if (time_unix(tm) != c->unix || time_nanosecond(tm) != c->got_nsec ||
            !str_eq(zn, cstr(c->zone)) || off != c->offset)
            testing_t_errorf_v(
                t,
                "Date(%d, %d, %d, %d, %d, %d, %d, %s) = %d.%d %q %d, want "
                "%d.%d %q %d",
                c->year, c->month, c->day, c->hour, c->min, c->sec, c->nsec,
                time_location_string(loc_of(c->loc)), time_unix(tm),
                time_nanosecond(tm), zn, off, c->unix, c->got_nsec, c->zone, c->offset);
    }
}

static void TestAddDate(TestingT *t) {
    load_locs(t);
    for (size_t i = 0; i < LEN(g_adds); i++) {
        const ACase *c = &g_adds[i];
        Time tm = time_add_date(time_in(time_from_unix(c->base, 123), loc_of(c->loc)),
                                c->years, c->months, c->days);
        if (time_unix(tm) != c->unix || time_nanosecond(tm) != c->nsec)
            testing_t_errorf_v(t, "%s %d AddDate(%d, %d, %d) = %d.%d, want %d.%d",
                               time_location_string(loc_of(c->loc)), c->base, c->years,
                               c->months, c->days, time_unix(tm), time_nanosecond(tm),
                               c->unix, c->nsec);
    }
}

static void TestTruncateRound(TestingT *t) {
    for (size_t i = 0; i < LEN(g_rounds); i++) {
        const RCase *c = &g_rounds[i];
        Time tm = time_from_unix(c->sec, c->nsec);
        Time tt = time_truncate(tm, c->d);
        Time tr = time_round(tm, c->d);
        if (time_unix(tt) != c->trunc_sec || time_nanosecond(tt) != c->trunc_nsec)
            testing_t_errorf_v(t, "%d.%d Truncate(%d) = %d.%d, want %d.%d", c->sec,
                               c->nsec, c->d, time_unix(tt), time_nanosecond(tt),
                               c->trunc_sec, c->trunc_nsec);
        if (time_unix(tr) != c->round_sec || time_nanosecond(tr) != c->round_nsec)
            testing_t_errorf_v(t, "%d.%d Round(%d) = %d.%d, want %d.%d", c->sec,
                               c->nsec, c->d, time_unix(tr), time_nanosecond(tr),
                               c->round_sec, c->round_nsec);
    }
}

static void TestSubCompare(TestingT *t) {
    for (size_t i = 0; i < LEN(g_subs); i++) {
        const SCase *c = &g_subs[i];
        Time a = time_from_unix(c->a, c->an);
        Time b = time_from_unix(c->b, c->bn);
        Duration d = time_sub(a, b);
        if (d != c->sub)
            testing_t_errorf_v(t, "%d.%d - %d.%d = %d, want %d", c->a, c->an, c->b,
                               c->bn, d, c->sub);
        if (time_compare(a, b) != c->cmp || time_before(a, b) != (c->cmp < 0) ||
            time_after(a, b) != (c->cmp > 0) || time_equal(a, b) != (c->cmp == 0))
            testing_t_errorf_v(t, "%d.%d vs %d.%d: Compare = %d, want %d", c->a, c->an,
                               c->b, c->bn, time_compare(a, b), c->cmp);
    }
}

static void TestAdd(TestingT *t) {
    for (size_t i = 0; i < LEN(g_add); i++) {
        const AddCase *c = &g_add[i];
        Time tm = time_add(time_from_unix(c->sec, c->nsec), c->d);
        if (time_unix(tm) != c->unix || time_nanosecond(tm) != c->got_nsec)
            testing_t_errorf_v(t, "%d.%d + %d = %d.%d, want %d.%d", c->sec, c->nsec,
                               c->d, time_unix(tm), time_nanosecond(tm), c->unix,
                               c->got_nsec);
    }
}

static void TestDurationMethods(TestingT *t) {
    for (size_t i = 0; i < LEN(g_durs); i++) {
        const DurCase *c = &g_durs[i];
        Duration d = c->d;
        if (duration_nanoseconds(d) != c->ns || duration_microseconds(d) != c->us ||
            duration_milliseconds(d) != c->ms || duration_abs(d) != c->abs)
            testing_t_errorf_v(t, "%d: integer methods wrong", d);
        if (duration_seconds(d) != c->s || duration_minutes(d) != c->m ||
            duration_hours(d) != c->h)
            testing_t_errorf_v(t, "%d: Seconds %v Minutes %v Hours %v, want %v %v %v",
                               d, duration_seconds(d), duration_minutes(d),
                               duration_hours(d), c->s, c->m, c->h);
    }
    for (size_t i = 0; i < LEN(g_dur_rounds); i++) {
        const DurRound *c = &g_dur_rounds[i];
        Duration tr = duration_truncate(c->d, c->m);
        Duration ro = duration_round(c->d, c->m);
        if (tr != c->trunc || ro != c->round)
            testing_t_errorf_v(t, "%d Truncate(%d) = %d Round = %d, want %d %d", c->d,
                               c->m, tr, ro, c->trunc, c->round);
    }
}

static void TestMonthWeekdayString(TestingT *t) {
    for (size_t i = 0; i < LEN(g_months); i++) {
        Str s = time_month_string(g_months[i].v, heap_allocator());
        CHECK(str_eq(s, cstr(g_months[i].want)));
    }
    for (size_t i = 0; i < LEN(g_weekdays); i++) {
        Str s = time_weekday_string(g_weekdays[i].v, heap_allocator());
        CHECK(str_eq(s, cstr(g_weekdays[i].want)));
    }
}

static void TestUnmarshalBinaryBad(TestingT *t) {
    for (size_t i = 0; i < LEN(g_bins); i++) {
        const BinCase *c = &g_bins[i];
        Time tm = {0, 0, NULL};
        Error err = time_unmarshal_binary(&tm, heap_allocator(), blob(c->data, c->len));
        Str want = cstr(c->err == NULL ? "<nil>" : c->err);
        if (!str_eq(err_str(err), want)) {
            testing_t_errorf_v(t, "case %d: error %q, want %q", (Int)i, err_str(err),
                               want);
            continue;
        }
        Int off = 0;
        Str zn = time_zone(tm, &off);
        if (time_unix(tm) != c->unix || time_nanosecond(tm) != c->nsec ||
            !str_eq(zn, cstr(c->zone)) || off != c->offset)
            testing_t_errorf_v(t, "case %d: got %d.%d %q %d, want %d.%d %q %d", (Int)i,
                               time_unix(tm), time_nanosecond(tm), zn, off, c->unix,
                               c->nsec, c->zone, c->offset);
        time_location_free(time_location(tm));
    }
}

static void TestTzset(TestingT *t) {
    for (size_t i = 0; i < LEN(g_tzset); i++) {
        const TzsetCase *c = &g_tzset[i];
        TzLookup r = {{NULL, 0}, 0, 0, 0, false};
        bool ok = burrow__time_tzset(cstr(c->s), c->last, c->sec, &r);
        if (ok != (c->ok != 0)) {
            testing_t_errorf_v(t, "tzset(%q, %d, %d) ok = %v", c->s, c->last, c->sec,
                               ok);
            continue;
        }
        if (ok &&
            (!str_eq(r.name, cstr(c->name)) || r.offset != c->off ||
             r.start != c->start || r.end != c->end || r.is_dst != (c->is_dst != 0)))
            testing_t_errorf_v(
                t, "tzset(%q, %d, %d) = %q %d %d %d %v, want %q %d %d %d %v", c->s,
                c->last, c->sec, r.name, r.offset, r.start, r.end, r.is_dst, c->name,
                c->off, c->start, c->end, c->is_dst != 0);
    }
    for (size_t i = 0; i < LEN(g_tzset_names); i++) {
        const TzsetNameCase *c = &g_tzset_names[i];
        Str name = {NULL, 0};
        Str rest = {NULL, 0};
        bool ok = burrow__time_tzset_name(cstr(c->s), &name, &rest);
        if (ok != (c->ok != 0) ||
            (ok && (!str_eq(name, cstr(c->name)) || !str_eq(rest, cstr(c->rest)))))
            testing_t_errorf_v(t, "tzsetName(%q) = %q %q %v, want %q %q %v", c->s, name,
                               rest, ok, c->name, c->rest, c->ok != 0);
    }
    for (size_t i = 0; i < LEN(g_tzset_offsets); i++) {
        const TzsetOffsetCase *c = &g_tzset_offsets[i];
        Int off = 0;
        Str rest = {NULL, 0};
        bool ok = burrow__time_tzset_offset(cstr(c->s), &off, &rest);
        if (ok != (c->ok != 0) ||
            (ok && (off != c->off || !str_eq(rest, cstr(c->rest)))))
            testing_t_errorf_v(t, "tzsetOffset(%q) = %d %q %v, want %d %q %v", c->s,
                               off, rest, ok, c->off, c->rest, c->ok != 0);
    }
    for (size_t i = 0; i < LEN(g_tzset_rules); i++) {
        const TzsetRuleCase *c = &g_tzset_rules[i];
        TzRule r = {0, 0, 0, 0, 0};
        Str rest = {NULL, 0};
        bool ok = burrow__time_tzset_rule(cstr(c->s), &r, &rest);
        if (ok != (c->ok != 0) ||
            (ok &&
             (r.kind != c->kind || r.day != c->day || r.week != c->week ||
              r.mon != c->mon || r.time != c->time || !str_eq(rest, cstr(c->rest)))))
            testing_t_errorf_v(t, "tzsetRule(%q) = %d %d %d %d %d %q %v", c->s, r.kind,
                               r.day, r.week, r.mon, r.time, rest, ok);
    }
}

static void TestBadTZData(TestingT *t) {
    for (size_t i = 0; i < LEN(g_bad); i++) {
        const BadCase *c = &g_bad[i];
        const TzBlob *z = &g_zones[c->zone];
        Byte buf[4096];
        if (z->len > (long long)sizeof buf)
            testing_t_fatalf_v(t, "%s is too big for the buffer", z->name);
        memcpy(buf, z->data, (size_t)z->len);
        if (c->pos >= 0)
            buf[c->pos] ^= (Byte)c->x;
        Error err = BURROW_NO_ERROR;
        TimeLocation *l = time_load_location_from_tz_data(
            heap_allocator(), cstr("bad"), slice_from(buf, c->cut, c->cut, TYPE_BYTE),
            &err);
        Str want = cstr(c->err == NULL ? "<nil>" : c->err);
        if (!str_eq(err_str(err), want) || (l == NULL) != (c->err != NULL)) {
            testing_t_errorf_v(t, "%s cut %d flip %d: error %q, want %q", z->name,
                               c->cut, c->pos, err_str(err), want);
            time_location_free(l);
            continue;
        }
        if (l == NULL)
            continue;
        for (int k = 0; k < 3; k++) {
            Int off = 0;
            Str zn = time_zone(time_in(time_from_unix(g_probe[k], 0), l), &off);
            if (!str_eq(zn, cstr(c->zname[k])) || off != c->zoff[k])
                testing_t_errorf_v(t, "%s cut %d flip %d at %d: %q %d, want %q %d",
                                   z->name, c->cut, c->pos, g_probe[k], zn, off,
                                   c->zname[k], c->zoff[k]);
        }
        time_location_free(l);
    }

    Error err = BURROW_NO_ERROR;
    TimeLocation *l = time_load_location_from_tz_data(
        heap_allocator(), cstr("abc"),
        blob(g_issue29437, (long long)sizeof g_issue29437 - 1), &err);
    CHECK(l == NULL);
    CHECK(str_eq(err_str(err), cstr(g_issue29437_err)));
}

static void TestLoadLocationValidatesNames(TestingT *t) {
    static const char *const bad[] = {"/usr/foo/Foo", "\\UNC\\foo", "..",
                                      "a..",          "../foo",     "foo/..",
                                      "./foo",        "foo/.",      "../../etc/passwd"};
    for (size_t i = 0; i < LEN(bad); i++) {
        Error err = BURROW_NO_ERROR;
        TimeLocation *l = time_load_location(cstr(bad[i]), &err);
        if (i < 6 || i == 8) {
            if (l != NULL || !str_eq(err_str(err), cstr("time: invalid location name")))
                testing_t_errorf_v(t, "LoadLocation(%q) error %q", bad[i],
                                   err_str(err));
        } else if (l != NULL) {
            testing_t_errorf_v(t, "LoadLocation(%q) found something", bad[i]);
        }
    }
    Error err = BURROW_NO_ERROR;
    CHECK(time_load_location(cstr(""), &err) == time_utc_loc);
    CHECK(time_load_location(cstr("UTC"), &err) == time_utc_loc);
    CHECK(time_load_location(cstr("Local"), &err) == time_local_loc);
    CHECK(time_load_location(cstr("Mars/Olympus_Mons"), &err) == NULL);
    CHECK(str_eq(err_str(err), cstr("unknown time zone Mars/Olympus_Mons")));
}

/* The system's own zoneinfo, when there is one, agrees with Go's about the
 * past, and a second load of a name is the first one again. */
static void TestLoadLocationSystem(TestingT *t) {
    load_locs(t);
    Error err = BURROW_NO_ERROR;
    TimeLocation *ny = time_load_location(cstr("America/New_York"), &err);
    if (ny == NULL)
        testing_t_skipf_v(t, "no zoneinfo here: %s", err_str(err));
    CHECK(time_load_location(cstr("America/New_York"), &err) == ny);
    CHECK(str_eq(time_location_string(ny), cstr("America/New_York")));
    static const int64_t when[] = {INT64_C(-2000000000), 0, 1000000000, 1234567890,
                                   1700000000};
    for (size_t i = 0; i < LEN(when); i++) {
        Int o1 = 0;
        Int o2 = 0;
        Str z1 = time_zone(time_in(time_from_unix(when[i], 0), ny), &o1);
        Str z2 = time_zone(time_in(time_from_unix(when[i], 0), g_locs[0]), &o2);
        if (!str_eq(z1, z2) || o1 != o2)
            testing_t_errorf_v(t, "%d: system says %q %d, Go says %q %d", when[i], z1,
                               o1, z2, o2);
    }
}

/* Local follows $TZ, which the process was started with and which Local reads
 * once. Run the test under TZ=, TZ=UTC, TZ=America/New_York and no TZ at all
 * to see each branch. */
static void TestLocal(TestingT *t) {
    const char *tz = getenv("TZ");
    Str name = time_location_string(time_local_loc);
    Int off = 0;
    Str zn = time_zone(time_in(time_from_unix(1000000000, 0), time_local_loc), &off);
    if (tz == NULL) {
        CHECK(str_eq(name, cstr("Local")) || str_eq(name, cstr("UTC")));
        return;
    }
    if (tz[0] == ':')
        tz++;
    if (tz[0] == '\0' || strcmp(tz, "UTC") == 0) {
        CHECK(str_eq(name, cstr("UTC")));
        CHECK(str_eq(zn, cstr("UTC")));
        CHECK_INT_EQ(off, 0);
        return;
    }
    if (strcmp(tz, "America/New_York") == 0 &&
        time_load_location(cstr("America/New_York"), NULL) != NULL) {
        CHECK(str_eq(name, cstr("America/New_York")));
        CHECK(str_eq(zn, cstr("EDT")));
        CHECK_INT_EQ(off, -4 * 3600);
    }
    if (strcmp(tz, "/etc/localtime") == 0)
        CHECK(str_eq(name, cstr("Local")) || str_eq(name, cstr("UTC")));
}

static void TestNow(TestingT *t) {
    Time a = time_now();
    Time b = time_now();
    CHECK(!time_before(b, a));
    CHECK(time_sub(b, a) >= 0);
    CHECK(time_since(a) >= 0);
    CHECK(time_until(a) <= 0);
    /* 2020 to 2200, which is a sanity check on the wall clock and not a
     * test of it. */
    CHECK(time_unix(a) > 1577836800 && time_unix(a) < 7258118400);
    /* The monotonic reading goes when the location changes. */
    Time u = time_utc(a);
    CHECK(time_equal(u, a));
    CHECK(u.ext != a.ext || (a.wall >> 63) == 0);
}

static void TestZeroTime(TestingT *t) {
    Time z = {0, 0, NULL};
    CHECK(time_is_zero(z));
    CHECK_INT_EQ(time_year(z), 1);
    CHECK_INT_EQ(time_month(z), TIME_JANUARY);
    CHECK_INT_EQ(time_day(z), 1);
    CHECK_INT_EQ(time_weekday(z), TIME_MONDAY);
    CHECK_INT_EQ(time_unix(z), INT64_C(-62135596800));
    CHECK(time_is_zero(time_date(1, TIME_JANUARY, 1, 0, 0, 0, 0, time_utc_loc)));
    CHECK(!time_is_zero(time_from_unix(0, 0)));
}

static void TestFixedZone(TestingT *t) {
    TimeLocation *a = time_fixed_zone(heap_allocator(), cstr(""), 3600);
    TimeLocation *b = time_fixed_zone(heap_allocator(), cstr(""), 3600);
    CHECK(a == b); /* from the table */
    time_location_free(a);
    TimeLocation *c = time_fixed_zone(heap_allocator(), cstr("ABC"), 3600);
    CHECK(c != a);
    Int off = 0;
    Str zn = time_zone(time_in(time_from_unix(0, 0), c), &off);
    CHECK(str_eq(zn, cstr("ABC")));
    CHECK_INT_EQ(off, 3600);
    time_location_free(c);
}

#define TESTS(X)                                                                       \
    X(TestLocationNames)                                                               \
    X(TestTimes)                                                                       \
    X(TestDate)                                                                        \
    X(TestAddDate)                                                                     \
    X(TestTruncateRound)                                                               \
    X(TestSubCompare)                                                                  \
    X(TestAdd)                                                                         \
    X(TestDurationMethods)                                                             \
    X(TestMonthWeekdayString)                                                          \
    X(TestUnmarshalBinaryBad)                                                          \
    X(TestTzset)                                                                       \
    X(TestBadTZData)                                                                   \
    X(TestLoadLocationValidatesNames)                                                  \
    X(TestLoadLocationSystem)                                                          \
    X(TestLocal)                                                                       \
    X(TestNow)                                                                         \
    X(TestZeroTime)                                                                    \
    X(TestFixedZone)

TESTING_MAIN(TESTS)
