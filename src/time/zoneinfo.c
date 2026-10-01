/* Locations: Go's zoneinfo.go, zoneinfo_read.go and zoneinfo_unix.go.
 *
 * A location is a list of zones, each an abbreviation and an offset, and a
 * list of transitions saying from when on each zone is in effect. The lists
 * come from a TZif file, the compiled form of the IANA database that every
 * Unix keeps under /usr/share/zoneinfo. After the last transition the file
 * may carry a POSIX TZ string such as "CET-1CEST,M3.5.0,M10.5.0/3", which
 * says how the zone carries on for ever, and tzset reads that.
 *
 * Go reads the file every time LoadLocation is called and leaves the result
 * to the collector. Here a loaded location is kept for the life of the process
 * and the second load of a name hands back the first one, because a Time
 * points at its location and something has to own it. The number of names is
 * bounded by the database, which is a few hundred.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/time.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/pal.h"
#include "burrow/platform.h"
#include "burrow/sync.h"

#include "time_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TZ_ADD(a, b) ((int64_t)((uint64_t)(a) + (uint64_t)(b)))
#define TZ_SUB(a, b) ((int64_t)((uint64_t)(a) - (uint64_t)(b)))

static Str tz_cstr(const char *s) {
    return (Str){(const Byte *)s, (Int)strlen(s)};
}

/* a + b, from the allocator errors come from, for the texts of errors. */
static Str tz_concat(Str a, Str b) {
    Byte *p = mem_alloc(error_allocator(), (size_t)(a.len + b.len + 1), 1);
    if (p == NULL)
        return a;
    if (a.len > 0)
        memcpy(p, a.p, (size_t)a.len);
    if (b.len > 0)
        memcpy(p + a.len, b.p, (size_t)b.len);
    return (Str){p, a.len + b.len};
}

static Error tz_err(Str s) {
    return errors_new(error_allocator(), s);
}

/* ------------------------------------------------------ the fixed ones */

/* UTC, which has no zones at all and answers "UTC" with offset 0 for every
 * instant, and Local, filled in once by tz_init_local. Neither is ever freed,
 * and UTC is never written. */
static TimeLocation tz_utc_loc = {
    {(const Byte *)"UTC", 3}, NULL, 0, NULL, 0, {NULL, 0}, 0, 0, NULL,
    {{NULL, 0}, 0, false},    NULL, 0, NULL};
static TimeLocation tz_local_loc;
static SyncOnce tz_local_once;

TimeLocation *const time_utc_loc = &tz_utc_loc;
TimeLocation *const time_local_loc = &tz_local_loc;

TimeLocation *burrow__time_utc(void) {
    return &tz_utc_loc;
}

static void tz_init_local(void *arg);

TimeLocation *burrow__time_loc_get(TimeLocation *l) {
    if (l == NULL)
        return &tz_utc_loc;
    if (l == &tz_local_loc)
        sync_once_do(&tz_local_once, BURROW_FN(Func, tz_init_local, NULL));
    return l;
}

Str time_location_string(TimeLocation *l) {
    return burrow__time_loc_get(l)->name;
}

/* The unnamed zones at whole hours from -12 to +14, which Go makes once and
 * hands out again, so that Parse and the binary decoding do not make a new
 * location for every value that says +01:00. */
enum { TZ_HOURS_BEFORE_UTC = 12, TZ_HOURS_AFTER_UTC = 14 };
#define TZ_NFIXED (TZ_HOURS_BEFORE_UTC + 1 + TZ_HOURS_AFTER_UTC)

static const TzTrans tz_fixed_tx[1] = {{TZ_ALPHA, 0, false, false}};

#define TZ_FIXED_ZONE(h) {{NULL, 0}, (Int)(h) * TZ_SECONDS_PER_HOUR, false}
static const TzZone tz_fixed_zones[TZ_NFIXED] = {
    TZ_FIXED_ZONE(-12), TZ_FIXED_ZONE(-11), TZ_FIXED_ZONE(-10), TZ_FIXED_ZONE(-9),
    TZ_FIXED_ZONE(-8),  TZ_FIXED_ZONE(-7),  TZ_FIXED_ZONE(-6),  TZ_FIXED_ZONE(-5),
    TZ_FIXED_ZONE(-4),  TZ_FIXED_ZONE(-3),  TZ_FIXED_ZONE(-2),  TZ_FIXED_ZONE(-1),
    TZ_FIXED_ZONE(0),   TZ_FIXED_ZONE(1),   TZ_FIXED_ZONE(2),   TZ_FIXED_ZONE(3),
    TZ_FIXED_ZONE(4),   TZ_FIXED_ZONE(5),   TZ_FIXED_ZONE(6),   TZ_FIXED_ZONE(7),
    TZ_FIXED_ZONE(8),   TZ_FIXED_ZONE(9),   TZ_FIXED_ZONE(10),  TZ_FIXED_ZONE(11),
    TZ_FIXED_ZONE(12),  TZ_FIXED_ZONE(13),  TZ_FIXED_ZONE(14),
};

#define TZ_FIXED_LOC(i)                                                                \
    {{NULL, 0},                                                                        \
     &tz_fixed_zones[i],                                                               \
     1,                                                                                \
     tz_fixed_tx,                                                                      \
     1,                                                                                \
     {NULL, 0},                                                                        \
     TZ_ALPHA,                                                                         \
     TZ_OMEGA,                                                                         \
     &tz_fixed_zones[i],                                                               \
     {{NULL, 0}, 0, false},                                                            \
     NULL,                                                                             \
     0,                                                                                \
     NULL}
static const TimeLocation tz_fixed_locs[TZ_NFIXED] = {
    TZ_FIXED_LOC(0),  TZ_FIXED_LOC(1),  TZ_FIXED_LOC(2),  TZ_FIXED_LOC(3),
    TZ_FIXED_LOC(4),  TZ_FIXED_LOC(5),  TZ_FIXED_LOC(6),  TZ_FIXED_LOC(7),
    TZ_FIXED_LOC(8),  TZ_FIXED_LOC(9),  TZ_FIXED_LOC(10), TZ_FIXED_LOC(11),
    TZ_FIXED_LOC(12), TZ_FIXED_LOC(13), TZ_FIXED_LOC(14), TZ_FIXED_LOC(15),
    TZ_FIXED_LOC(16), TZ_FIXED_LOC(17), TZ_FIXED_LOC(18), TZ_FIXED_LOC(19),
    TZ_FIXED_LOC(20), TZ_FIXED_LOC(21), TZ_FIXED_LOC(22), TZ_FIXED_LOC(23),
    TZ_FIXED_LOC(24), TZ_FIXED_LOC(25), TZ_FIXED_LOC(26),
};

TimeLocation *burrow__time_fixed_zone(Alloc *a, Str name, Int offset) {
    Int hour = offset / 60 / 60;
    if (name.len == 0 && -TZ_HOURS_BEFORE_UTC <= hour && hour <= TZ_HOURS_AFTER_UTC &&
        hour * 60 * 60 == offset)
        /* Never written: the table is const only as far as C can say so. */
        return (TimeLocation *)(uintptr_t)&tz_fixed_locs[hour + TZ_HOURS_BEFORE_UTC];

    size_t size =
        sizeof(TimeLocation) + sizeof(TzZone) + sizeof(TzTrans) + (size_t)name.len;
    TimeLocation *l = mem_alloc(a, size, _Alignof(TimeLocation));
    if (l == NULL)
        return NULL;
    TzZone *z = (TzZone *)(void *)(l + 1);
    TzTrans *tx = (TzTrans *)(void *)(z + 1);
    Byte *p = (Byte *)(tx + 1);
    if (name.len > 0)
        memcpy(p, name.p, (size_t)name.len);
    *z = (TzZone){{p, name.len}, offset, false};
    *tx = (TzTrans){TZ_ALPHA, 0, false, false};
    l->name = z->name;
    l->zone = z;
    l->nzone = 1;
    l->tx = tx;
    l->ntx = 1;
    l->cache_start = TZ_ALPHA;
    l->cache_end = TZ_OMEGA;
    l->cache_zone = z;
    l->a = a;
    l->size = size;
    return l;
}

TimeLocation *time_fixed_zone(Alloc *a, Str name, Int offset) {
    return burrow__time_fixed_zone(a, name, offset);
}

void time_location_free(TimeLocation *l) {
    if (l == NULL || l->a == NULL)
        return;
    mem_free(l->a, l, l->size, _Alignof(TimeLocation));
}

/* ----------------------------------------------------------------- lookup */

static Int tz_first_zone(const TimeLocation *l);

TzLookup burrow__time_lookup(TimeLocation *l, int64_t sec) {
    TzLookup r;
    l = burrow__time_loc_get(l);

    if (l->nzone == 0) {
        r.name = BURROW_S("UTC");
        r.offset = 0;
        r.start = TZ_ALPHA;
        r.end = TZ_OMEGA;
        r.is_dst = false;
        return r;
    }

    const TzZone *z = l->cache_zone;
    if (z != NULL && l->cache_start <= sec && sec < l->cache_end) {
        r.name = z->name;
        r.offset = z->offset;
        r.start = l->cache_start;
        r.end = l->cache_end;
        r.is_dst = z->is_dst;
        return r;
    }

    if (l->ntx == 0 || sec < l->tx[0].when) {
        z = &l->zone[tz_first_zone(l)];
        r.name = z->name;
        r.offset = z->offset;
        r.start = TZ_ALPHA;
        r.end = l->ntx > 0 ? l->tx[0].when : TZ_OMEGA;
        r.is_dst = z->is_dst;
        return r;
    }

    /* Binary search for the entry with the largest time <= sec. */
    const TzTrans *tx = l->tx;
    r.end = TZ_OMEGA;
    Int lo = 0;
    Int hi = l->ntx;
    while (hi - lo > 1) {
        Int m = (Int)((uint64_t)(lo + hi) >> 1);
        int64_t lim = tx[m].when;
        if (sec < lim) {
            r.end = lim;
            hi = m;
        } else {
            lo = m;
        }
    }
    z = &l->zone[tx[lo].index];
    r.name = z->name;
    r.offset = z->offset;
    r.start = tx[lo].when;
    r.is_dst = z->is_dst;

    /* After the last transition the TZ string, if there is one, says what
     * happens. */
    if (lo == l->ntx - 1 && l->extend.len > 0) {
        TzLookup e;
        if (burrow__time_tzset(l->extend, r.start, sec, &e))
            return e;
    }
    return r;
}

/* Go's lookupFirstZone: the zone for times before the first transition, which
 * is the first standard time zone, unless zone 0 is never used by a
 * transition, in which case zone 0. */
static Int tz_first_zone(const TimeLocation *l) {
    bool used = false;
    for (Int i = 0; i < l->ntx; i++) {
        if (l->tx[i].index == 0) {
            used = true;
            break;
        }
    }
    if (!used)
        return 0;

    /* The first zone before the first transition that is not DST. */
    if (l->ntx > 0 && l->zone[l->tx[0].index].is_dst) {
        for (Int zi = (Int)l->tx[0].index - 1; zi >= 0; zi--) {
            if (!l->zone[zi].is_dst)
                return zi;
        }
    }

    for (Int zi = 0; zi < l->nzone; zi++) {
        if (!l->zone[zi].is_dst)
            return zi;
    }
    return 0;
}

bool burrow__time_lookup_name(TimeLocation *l, Str name, int64_t unix, Int *offset) {
    l = burrow__time_loc_get(l);

    /* First the zone called name that is in effect at unix, if there is one,
     * which puts 2006-01-02 15:04:05 MST in the right half of a year with two
     * MSTs in it. */
    for (Int i = 0; i < l->nzone; i++) {
        const TzZone *z = &l->zone[i];
        if (str_eq(z->name, name)) {
            TzLookup r = burrow__time_lookup(l, TZ_SUB(unix, z->offset));
            if (str_eq(r.name, z->name)) {
                *offset = r.offset;
                return true;
            }
        }
    }

    /* Otherwise any zone of that name. */
    for (Int i = 0; i < l->nzone; i++) {
        const TzZone *z = &l->zone[i];
        if (str_eq(z->name, name)) {
            *offset = z->offset;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ tzset */

static Str tz_rest(Str s, Int i) {
    return (Str){s.p + i, s.len - i};
}

/* Go's tzsetNum: a decimal number between min and max. */
static bool tz_num(Str s, Int min, Int max, Int *num, Str *rest) {
    if (s.len == 0)
        return false;
    Int n = 0;
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        if (c < '0' || c > '9') {
            if (i == 0 || n < min)
                return false;
            *num = n;
            *rest = tz_rest(s, i);
            return true;
        }
        n = n * 10 + (c - '0');
        if (n > max)
            return false;
    }
    if (n < min)
        return false;
    *num = n;
    *rest = tz_rest(s, s.len);
    return true;
}

bool burrow__time_tzset_name(Str s, Str *name, Str *rest) {
    if (s.len == 0)
        return false;
    if (s.p[0] != '<') {
        for (Int i = 0; i < s.len; i++) {
            switch (s.p[i]) {
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
            case ',':
            case '-':
            case '+':
                if (i < 3)
                    return false;
                *name = (Str){s.p, i};
                *rest = tz_rest(s, i);
                return true;
            default:
                break;
            }
        }
        if (s.len < 3)
            return false;
        *name = s;
        *rest = tz_rest(s, s.len);
        return true;
    }
    for (Int i = 0; i < s.len; i++) {
        if (s.p[i] == '>') {
            *name = (Str){s.p + 1, i - 1};
            *rest = tz_rest(s, i + 1);
            return true;
        }
    }
    return false;
}

bool burrow__time_tzset_offset(Str s, Int *offset, Str *rest) {
    if (s.len == 0)
        return false;
    bool neg = false;
    if (s.p[0] == '+') {
        s = tz_rest(s, 1);
    } else if (s.p[0] == '-') {
        s = tz_rest(s, 1);
        neg = true;
    }

    /* The hours may go up to 24 * 7, which POSIX allows for a rule time. */
    Int hours;
    if (!tz_num(s, 0, (Int)24 * 7, &hours, &s))
        return false;
    Int off = hours * TZ_SECONDS_PER_HOUR;
    if (s.len == 0 || s.p[0] != ':') {
        *offset = neg ? -off : off;
        *rest = s;
        return true;
    }

    Int mins;
    if (!tz_num(tz_rest(s, 1), 0, 59, &mins, &s))
        return false;
    off += mins * TZ_SECONDS_PER_MINUTE;
    if (s.len == 0 || s.p[0] != ':') {
        *offset = neg ? -off : off;
        *rest = s;
        return true;
    }

    Int secs;
    if (!tz_num(tz_rest(s, 1), 0, 59, &secs, &s))
        return false;
    off += secs;
    *offset = neg ? -off : off;
    *rest = s;
    return true;
}

enum { TZ_RULE_JULIAN, TZ_RULE_DOY, TZ_RULE_MONTH_WEEK_DAY };

bool burrow__time_tzset_rule(Str s, TzRule *out, Str *rest) {
    TzRule r = {0, 0, 0, 0, 0};
    if (s.len == 0)
        return false;
    if (s.p[0] == 'J') {
        Int jday;
        if (!tz_num(tz_rest(s, 1), 1, 365, &jday, &s))
            return false;
        r.kind = TZ_RULE_JULIAN;
        r.day = jday;
    } else if (s.p[0] == 'M') {
        Int mon;
        if (!tz_num(tz_rest(s, 1), 1, 12, &mon, &s) || s.len == 0 || s.p[0] != '.')
            return false;
        Int week;
        if (!tz_num(tz_rest(s, 1), 1, 5, &week, &s) || s.len == 0 || s.p[0] != '.')
            return false;
        Int day;
        if (!tz_num(tz_rest(s, 1), 0, 6, &day, &s))
            return false;
        r.kind = TZ_RULE_MONTH_WEEK_DAY;
        r.day = day;
        r.week = week;
        r.mon = mon;
    } else {
        Int day;
        if (!tz_num(s, 0, 365, &day, &s))
            return false;
        r.kind = TZ_RULE_DOY;
        r.day = day;
    }

    if (s.len == 0 || s.p[0] != '/') {
        r.time = (Int)2 * TZ_SECONDS_PER_HOUR; /* 2am is the default */
        *out = r;
        *rest = s;
        return true;
    }

    Int offset;
    if (!burrow__time_tzset_offset(tz_rest(s, 1), &offset, &s))
        return false;
    r.time = offset;
    *out = r;
    *rest = s;
    return true;
}

/* Go's tzruleTime: the second of year at which rule r takes effect, in UTC
 * given that the offset in effect before it is off. */
static Int tz_rule_time(Int year, TzRule r, Int off) {
    Int s = 0;
    switch (r.kind) {
    case TZ_RULE_JULIAN:
        s = (r.day - 1) * TZ_SECONDS_PER_DAY;
        if (burrow__time_is_leap(year) && r.day >= 60)
            s += TZ_SECONDS_PER_DAY;
        break;
    case TZ_RULE_DOY:
        s = r.day * TZ_SECONDS_PER_DAY;
        break;
    case TZ_RULE_MONTH_WEEK_DAY: {
        /* Zeller's Congruence. */
        Int m1 = (r.mon + 9) % 12 + 1;
        Int yy0 = year;
        if (r.mon <= 2)
            yy0--;
        Int yy1 = yy0 / 100;
        Int yy2 = yy0 % 100;
        Int dow = ((26 * m1 - 2) / 10 + 1 + yy2 + yy2 / 4 + yy1 / 4 - 2 * yy1) % 7;
        if (dow < 0)
            dow += 7;
        /* Now dow is the day of the week of the first day of r.mon, and
         * d is the first day of the month that is day r.day. */
        Int d = r.day - dow;
        if (d < 0)
            d += 7;
        for (Int i = 1; i < r.week; i++) {
            if (d + 7 >= burrow__time_days_in(r.mon, year))
                break;
            d += 7;
        }
        d += burrow__time_days_before(r.mon);
        if (burrow__time_is_leap(year) && r.mon > 2)
            d++;
        s = d * TZ_SECONDS_PER_DAY;
        break;
    }
    default:
        break;
    }
    return s + r.time - off;
}

bool burrow__time_tzset(Str s, int64_t last_tx_sec, int64_t sec, TzLookup *out) {
    Str std_name;
    Str dst_name;
    Int std_offset;
    Int dst_offset;

    if (!burrow__time_tzset_name(s, &std_name, &s) ||
        !burrow__time_tzset_offset(s, &std_offset, &s))
        return false;

    /* The TZ definition does not say whether the offset is east or west of
     * UTC. It is west, so a string of EST5EDT is -5 hours. */
    std_offset = -std_offset;

    if (s.len == 0 || s.p[0] == ',') {
        /* No daylight saving time. */
        *out = (TzLookup){std_name, std_offset, last_tx_sec, TZ_OMEGA, false};
        return true;
    }

    if (!burrow__time_tzset_name(s, &dst_name, &s))
        return false;
    if (s.len == 0 || s.p[0] == ',') {
        dst_offset = std_offset + TZ_SECONDS_PER_HOUR;
    } else {
        if (!burrow__time_tzset_offset(s, &dst_offset, &s))
            return false;
        dst_offset = -dst_offset;
    }

    if (s.len == 0)
        /* The default DST rules per POSIX. */
        s = BURROW_S(",M3.2.0,M11.1.0");
    if (s.p[0] != ',' && s.p[0] != ';')
        return false;
    s = tz_rest(s, 1);

    TzRule start_rule;
    TzRule end_rule;
    if (!burrow__time_tzset_rule(s, &start_rule, &s) || s.len == 0 || s.p[0] != ',')
        return false;
    s = tz_rest(s, 1);
    if (!burrow__time_tzset_rule(s, &end_rule, &s) || s.len > 0)
        return false;

    Int year;
    Int yday;
    burrow__time_abs_year_yday((uint64_t)TZ_ADD(sec, TIME_UNIX_TO_ABSOLUTE) /
                                   TZ_SECONDS_PER_DAY,
                               &year, &yday);

    int64_t ysec = (yday - 1) * TZ_SECONDS_PER_DAY + sec % TZ_SECONDS_PER_DAY;

    /* The second the year starts, in UTC. */
    int64_t ystart = TZ_SUB(sec, ysec);

    int64_t start_sec = tz_rule_time(year, start_rule, std_offset);
    int64_t end_sec = tz_rule_time(year, end_rule, dst_offset);
    bool dst_is_dst = true;
    bool std_is_dst = false;
    /* In the southern hemisphere daylight saving time runs across the new
     * year, so the rules come the other way round. */
    if (end_sec < start_sec) {
        int64_t ts = start_sec;
        start_sec = end_sec;
        end_sec = ts;
        Str tn = std_name;
        std_name = dst_name;
        dst_name = tn;
        Int to = std_offset;
        std_offset = dst_offset;
        dst_offset = to;
        std_is_dst = true;
        dst_is_dst = false;
    }

    /* The start and end values that come back are accurate close to a
     * daylight saving time transition, and less so away from one. */
    if (ysec < start_sec)
        *out = (TzLookup){std_name, std_offset, ystart, TZ_ADD(start_sec, ystart),
                          std_is_dst};
    else if (ysec >= end_sec)
        *out =
            (TzLookup){std_name, std_offset, TZ_ADD(end_sec, ystart),
                       TZ_ADD(ystart, (int64_t)365 * TZ_SECONDS_PER_DAY), std_is_dst};
    else
        *out = (TzLookup){dst_name, dst_offset, TZ_ADD(start_sec, ystart),
                          TZ_ADD(end_sec, ystart), dst_is_dst};
    return true;
}

/* ------------------------------------------------------------------- TZif */

typedef struct TzData {
    const Byte *p;
    Int n;
    bool error;
} TzData;

static const Byte *tz_read(TzData *d, Int n) {
    if (d->n < n) {
        d->p = NULL;
        d->n = 0;
        d->error = true;
        return NULL;
    }
    const Byte *p = d->p;
    if (n > 0) {
        d->p += n;
        d->n -= n;
    }
    return p;
}

static bool tz_big4(TzData *d, uint32_t *n) {
    const Byte *p = tz_read(d, 4);
    if (p == NULL) {
        d->error = true;
        return false;
    }
    *n = (uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 |
         (uint32_t)p[0] << 24;
    return true;
}

static bool tz_big8(TzData *d, uint64_t *n) {
    uint32_t n1;
    uint32_t n2;
    bool ok1 = tz_big4(d, &n1);
    bool ok2 = tz_big4(d, &n2);
    if (!ok1 || !ok2) {
        d->error = true;
        return false;
    }
    *n = (uint64_t)n1 << 32 | n2;
    return true;
}

static bool tz_byte(TzData *d, Byte *b) {
    const Byte *p = tz_read(d, 1);
    if (p == NULL) {
        d->error = true;
        return false;
    }
    *b = p[0];
    return true;
}

static Error tz_bad_data(void) {
    return tz_err(BURROW_S("malformed time zone information"));
}

static Int tz_find_zone(const TzZone *zones, Int n, Str name, Int offset, bool is_dst) {
    for (Int i = 0; i < n; i++) {
        if (str_eq(zones[i].name, name) && zones[i].offset == offset &&
            zones[i].is_dst == is_dst)
            return i;
    }
    return -1;
}

enum { TZ_NUTC_LOCAL, TZ_NSTD_WALL, TZ_NLEAP, TZ_NTIME, TZ_NZONE, TZ_NCHAR };

static bool tz_counts(TzData *d, Int n[6]) {
    for (int i = 0; i < 6; i++) {
        uint32_t nn;
        if (!tz_big4(d, &nn))
            return false;
        n[i] = (Int)nn;
    }
    return true;
}

TimeLocation *time_load_location_from_tz_data(Alloc *a, Str name, Slice data,
                                              Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    TzData d = {data.p, data.len, false};

    /* 4-byte magic "TZif" */
    const Byte *magic = tz_read(&d, 4);
    if (magic == NULL || memcmp(magic, "TZif", 4) != 0) {
        BURROW_OUT(err, tz_bad_data());
        return NULL;
    }

    /* 1-byte version, then 15 bytes of padding */
    int version;
    const Byte *p = tz_read(&d, 16);
    if (p == NULL) {
        BURROW_OUT(err, tz_bad_data());
        return NULL;
    }
    switch (p[0]) {
    case 0:
        version = 1;
        break;
    case '2':
        version = 2;
        break;
    case '3':
        version = 3;
        break;
    default:
        BURROW_OUT(err, tz_bad_data());
        return NULL;
    }

    /* Six big-endian 32-bit integers: the number of UTC/local indicators,
     * of standard/wall indicators, of leap seconds, of transition times, of
     * local time zones, and of characters of time zone abbreviations. */
    Int n[6];
    if (!tz_counts(&d, n)) {
        BURROW_OUT(err, tz_bad_data());
        return NULL;
    }

    /* A version 2 or later file has the 32 bit data and then the same again
     * with 64 bit times. Skip to the 64 bit half. */
    bool is64 = false;
    if (version > 1) {
        Int skip = n[TZ_NTIME] * 4 + n[TZ_NTIME] + n[TZ_NZONE] * 6 + n[TZ_NCHAR] +
                   n[TZ_NLEAP] * 8 + n[TZ_NSTD_WALL] + n[TZ_NUTC_LOCAL];
        /* Skip the version 2 header that we just read. */
        skip += 4 + 16;
        (void)tz_read(&d, skip);

        is64 = true;

        /* Read the counts again, they can differ. */
        if (!tz_counts(&d, n)) {
            BURROW_OUT(err, tz_bad_data());
            return NULL;
        }
    }

    Int size = is64 ? 8 : 4;

    /* Transition times. */
    TzData txtimes = {tz_read(&d, n[TZ_NTIME] * size), n[TZ_NTIME] * size, false};

    /* Time zone indices for transition times. */
    const Byte *txzones = tz_read(&d, n[TZ_NTIME]);

    /* Zone info structures. */
    TzData zonedata = {tz_read(&d, n[TZ_NZONE] * 6), n[TZ_NZONE] * 6, false};

    /* Time zone abbreviations. */
    const Byte *abbrev = tz_read(&d, n[TZ_NCHAR]);

    /* Leap-second time pairs. */
    (void)tz_read(&d, n[TZ_NLEAP] * (size + 4));

    /* Whether tx times associated with local time types are specified as
     * standard time or wall time, and as UTC or local time. */
    const Byte *isstd = tz_read(&d, n[TZ_NSTD_WALL]);
    const Byte *isutc = tz_read(&d, n[TZ_NUTC_LOCAL]);

    if (d.error) { /* ran out of data */
        BURROW_OUT(err, tz_bad_data());
        return NULL;
    }

    Str extend = {NULL, 0};
    if (d.n > 2 && d.p[0] == '\n' && d.p[d.n - 1] == '\n')
        extend = (Str){d.p + 1, d.n - 2};

    /* Now we can build up a useful data structure. First the zone
     * information. utcoff[4] isdst[1] nameindex[1] */
    Int nzone = n[TZ_NZONE];
    if (nzone == 0) {
        /* Reject tzdata files with no zones. There is nothing useful in
         * them. */
        BURROW_OUT(err, tz_bad_data());
        return NULL;
    }
    Int ntx = n[TZ_NTIME] > 0 ? n[TZ_NTIME] : 1;
    Int nchar = n[TZ_NCHAR];

    size_t bytes = sizeof(TimeLocation) + (size_t)nzone * sizeof(TzZone) +
                   (size_t)ntx * sizeof(TzTrans) + (size_t)nchar + (size_t)name.len +
                   (size_t)extend.len;
    TimeLocation *l = mem_alloc(a, bytes, _Alignof(TimeLocation));
    if (l == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    TzZone *zones = (TzZone *)(void *)(l + 1);
    TzTrans *tx = (TzTrans *)(void *)(zones + nzone);
    Byte *chars = (Byte *)(tx + ntx);
    if (nchar > 0)
        memcpy(chars, abbrev, (size_t)nchar);
    Byte *pname = chars + nchar;
    if (name.len > 0)
        memcpy(pname, name.p, (size_t)name.len);
    Byte *pext = pname + name.len;
    if (extend.len > 0)
        memcpy(pext, extend.p, (size_t)extend.len);
    l->a = a;
    l->size = bytes;

    for (Int i = 0; i < nzone; i++) {
        uint32_t off;
        Byte b;
        if (!tz_big4(&zonedata, &off) || !tz_byte(&zonedata, &b))
            goto bad;
        zones[i].offset = (Int)(int32_t)off;
        zones[i].is_dst = b != 0;
        if (!tz_byte(&zonedata, &b) || (Int)b >= nchar)
            goto bad;
        const Byte *z = chars + b;
        const Byte *end = memchr(z, 0, (size_t)(nchar - b));
        zones[i].name = (Str){z, end != NULL ? (Int)(end - z) : nchar - (Int)b};
    }

    /* Now the transition time info. */
    for (Int i = 0; i < n[TZ_NTIME]; i++) {
        int64_t when;
        if (!is64) {
            uint32_t n4;
            if (!tz_big4(&txtimes, &n4))
                goto bad;
            when = (int64_t)(int32_t)n4;
        } else {
            uint64_t n8;
            if (!tz_big8(&txtimes, &n8))
                goto bad;
            when = (int64_t)n8;
        }
        tx[i].when = when;
        if ((Int)txzones[i] >= nzone)
            goto bad;
        tx[i].index = txzones[i];
        tx[i].isstd = i < n[TZ_NSTD_WALL] && isstd[i] != 0;
        tx[i].isutc = i < n[TZ_NUTC_LOCAL] && isutc[i] != 0;
    }

    if (n[TZ_NTIME] == 0)
        /* Build fake transition to cover all time. This happens in fixed
         * locations like "Etc/GMT0". */
        tx[0] = (TzTrans){TZ_ALPHA, 0, false, false};

    l->name = (Str){pname, name.len};
    l->zone = zones;
    l->nzone = nzone;
    l->tx = tx;
    l->ntx = ntx;
    l->extend = (Str){pext, extend.len};

    /* Fill in the cache with information about right now, since that will
     * be the most common lookup. */
    int64_t sec = time_unix(time_now());
    for (Int i = 0; i < ntx; i++) {
        if (tx[i].when <= sec && (i + 1 == ntx || sec < tx[i + 1].when)) {
            l->cache_start = tx[i].when;
            l->cache_end = TZ_OMEGA;
            l->cache_zone = &zones[tx[i].index];
            if (i + 1 < ntx) {
                l->cache_end = tx[i + 1].when;
            } else if (l->extend.len > 0) {
                /* If we're at the end of the known zone transitions, try
                 * the extend string. */
                TzLookup e;
                if (burrow__time_tzset(l->extend, l->cache_start, sec, &e)) {
                    l->cache_start = e.start;
                    l->cache_end = e.end;
                    /* Find the zone that is returned by tzset to avoid
                     * allocation if possible. */
                    Int zi = tz_find_zone(zones, nzone, e.name, e.offset, e.is_dst);
                    if (zi != -1) {
                        l->cache_zone = &zones[zi];
                    } else {
                        l->cache_own = (TzZone){e.name, e.offset, e.is_dst};
                        l->cache_zone = &l->cache_own;
                    }
                }
            }
            break;
        }
    }
    return l;

bad:
    mem_free(a, l, bytes, _Alignof(TimeLocation));
    BURROW_OUT(err, tz_bad_data());
    return NULL;
}

/* ---------------------------------------------------------- reading files */

enum { TZ_MAX_FILE_SIZE = 10 << 20 };

/* The result of looking for a file: the bytes, or the error, and whether the
 * error was that the file was not there, which the search skips over. */
typedef struct TzFile {
    Byte *p;
    Int len;
    Error err;
    bool enoent;
} TzFile;

static TzFile tz_fail(Error e) {
    TzFile f = {NULL, 0, e, false};
    return f;
}

static TzFile tz_errno(PalErrno e) {
    TzFile f = tz_fail(tz_err(tz_cstr(pal_errno_string(e))));
    f.enoent = e == PAL_ENOENT;
    return f;
}

static TzFile tz_not_found(void) {
    TzFile f = tz_fail(tz_err(tz_cstr(pal_errno_string(PAL_ENOENT))));
    f.enoent = true;
    return f;
}

static void tz_file_free(TzFile *f) {
    if (f->p != NULL)
        mem_free(heap_allocator(), f->p, (size_t)f->len, 1);
    f->p = NULL;
}

/* dir + "/" + name, NUL terminated, from the heap. */
static char *tz_path(Str dir, Str name, size_t *size) {
    size_t n = (size_t)dir.len + (dir.len > 0 ? 1 : 0) + (size_t)name.len + 1;
    char *p = mem_alloc(heap_allocator(), n, 1);
    if (p == NULL)
        return NULL;
    size_t k = 0;
    if (dir.len > 0) {
        memcpy(p, dir.p, (size_t)dir.len);
        k = (size_t)dir.len;
        p[k++] = '/';
    }
    if (name.len > 0)
        memcpy(p + k, name.p, (size_t)name.len);
    p[k + (size_t)name.len] = '\0';
    *size = n;
    return p;
}

static TzFile tz_read_file(Str path) {
    size_t psize;
    char *cpath = tz_path(BURROW_S(""), path, &psize);
    if (cpath == NULL)
        return tz_fail(burrow_err_out_of_memory);
    PalErrno e = 0;
    int64_t fd = pal_open(cpath, PAL_O_RDONLY, 0, &e);
    mem_free(heap_allocator(), cpath, psize, 1);
    if (fd < 0)
        return tz_errno(e);

    Alloc *h = heap_allocator();
    Byte *ret = NULL;
    Int len = 0;
    Int cap = 0;
    TzFile out;
    for (;;) {
        Byte buf[4096];
        int64_t n = pal_read(fd, buf, (int64_t)sizeof buf, &e);
        if (n > 0) {
            if (len + n > cap) {
                Int ncap = cap == 0 ? 8192 : cap * 2;
                while (ncap < len + n)
                    ncap *= 2;
                Byte *np = mem_realloc(h, ret, (size_t)cap, (size_t)ncap, 1);
                if (np == NULL) {
                    out = tz_fail(burrow_err_out_of_memory);
                    goto done;
                }
                ret = np;
                cap = ncap;
            }
            memcpy(ret + len, buf, (size_t)n);
            len += n;
        }
        if (n < 0) {
            out = tz_errno(e);
            goto done;
        }
        if (n == 0)
            break;
        if (len > TZ_MAX_FILE_SIZE) {
            Str parts = tz_concat(BURROW_S("time: file "), path);
            out = tz_fail(tz_err(tz_concat(parts, BURROW_S(" is too large"))));
            goto done;
        }
    }
    (void)pal_close(fd, NULL);
    out = (TzFile){ret, len, BURROW_NO_ERROR, false};
    if (ret != NULL && cap != len) {
        Byte *np = mem_realloc(h, ret, (size_t)cap, (size_t)len, 1);
        if (np != NULL)
            out.p = np;
        else
            out.len = cap; /* keep the size the block really has */
    }
    return out;

done:
    (void)pal_close(fd, NULL);
    if (ret != NULL)
        mem_free(h, ret, (size_t)cap, 1);
    return out;
}

static Int tz_get4(const Byte *b, Int n) {
    if (n < 4)
        return 0;
    return (Int)b[0] | (Int)b[1] << 8 | (Int)b[2] << 16 | (Int)b[3] << 24;
}

static Int tz_get2(const Byte *b, Int n) {
    if (n < 2)
        return 0;
    return (Int)b[0] | (Int)b[1] << 8;
}

/* n bytes at off, or at off from the end when off is negative. */
static bool tz_preadn(int64_t fd, Byte *buf, Int n, Int off) {
    if (off < 0) {
        int64_t end = pal_seek(fd, 0, 2, NULL);
        if (end < 0)
            return false;
        off += end;
        if (off < 0)
            return false;
    }
    while (n > 0) {
        int64_t m = pal_pread(fd, buf, n, off, NULL);
        if (m <= 0)
            return false;
        buf += m;
        n -= m;
        off += m;
    }
    return true;
}

/* Go's loadTzinfoFromZip: the uncompressed entry called name in the zip file,
 * which is how Go ships lib/time/zoneinfo.zip. */
static TzFile tz_load_from_zip(Str zipfile, Str name) {
    enum {
        ZECHEADER = 0x06054b50,
        ZCHEADER = 0x02014b50,
        ZTAILSIZE = 22,
        ZHEADERSIZE = 30,
        ZHEADER = 0x04034b50,
    };
    Alloc *h = heap_allocator();
    size_t psize;
    char *cpath = tz_path(BURROW_S(""), zipfile, &psize);
    if (cpath == NULL)
        return tz_fail(burrow_err_out_of_memory);
    PalErrno e = 0;
    int64_t fd = pal_open(cpath, PAL_O_RDONLY, 0, &e);
    mem_free(h, cpath, psize, 1);
    if (fd < 0)
        return tz_errno(e);

    Str corrupt = tz_concat(BURROW_S("corrupt zip file "), zipfile);
    TzFile out = tz_fail(tz_err(corrupt));
    Byte tail[ZTAILSIZE];
    Byte *dir = NULL;
    Int dsize = 0;
    if (!tz_preadn(fd, tail, ZTAILSIZE, -ZTAILSIZE) ||
        tz_get4(tail, ZTAILSIZE) != ZECHEADER)
        goto done;
    Int n = tz_get2(tail + 10, ZTAILSIZE - 10);
    dsize = tz_get4(tail + 12, ZTAILSIZE - 12);
    Int off = tz_get4(tail + 16, ZTAILSIZE - 16);

    dir = mem_alloc(h, (size_t)(dsize > 0 ? dsize : 1), 1);
    if (dir == NULL) {
        out = tz_fail(burrow_err_out_of_memory);
        goto done;
    }
    if (!tz_preadn(fd, dir, dsize, off))
        goto done;

    const Byte *buf = dir;
    Int blen = dsize;
    for (Int i = 0; i < n; i++) {
        /* Central directory entry. */
        if (tz_get4(buf, blen) != ZCHEADER)
            break;
        if (blen < 46)
            goto done;
        Int meth = tz_get2(buf + 10, blen - 10);
        Int size = tz_get4(buf + 24, blen - 24);
        Int namelen = tz_get2(buf + 28, blen - 28);
        Int xlen = tz_get2(buf + 30, blen - 30);
        Int fclen = tz_get2(buf + 32, blen - 32);
        off = tz_get4(buf + 42, blen - 42);
        if (46 + namelen + xlen + fclen > blen)
            goto done;
        Str zname = {buf + 46, namelen};
        buf += 46 + namelen + xlen + fclen;
        blen -= 46 + namelen + xlen + fclen;
        if (!str_eq(zname, name))
            continue;
        if (meth != 0) {
            Str s = tz_concat(BURROW_S("unsupported compression for "), name);
            s = tz_concat(s, BURROW_S(" in "));
            out = tz_fail(tz_err(tz_concat(s, zipfile)));
            goto done;
        }

        /* Local file header. */
        Byte hdr[ZHEADERSIZE];
        Byte *hname = mem_alloc(h, (size_t)(namelen > 0 ? namelen : 1), 1);
        if (hname == NULL) {
            out = tz_fail(burrow_err_out_of_memory);
            goto done;
        }
        bool ok = tz_preadn(fd, hdr, ZHEADERSIZE, off) &&
                  tz_preadn(fd, hname, namelen, off + ZHEADERSIZE) &&
                  tz_get4(hdr, ZHEADERSIZE) == ZHEADER &&
                  tz_get2(hdr + 8, ZHEADERSIZE - 8) == meth &&
                  tz_get2(hdr + 26, ZHEADERSIZE - 26) == namelen &&
                  str_eq((Str){hname, namelen}, name);
        mem_free(h, hname, (size_t)(namelen > 0 ? namelen : 1), 1);
        if (!ok)
            goto done;
        xlen = tz_get2(hdr + 28, ZHEADERSIZE - 28);

        Byte *data = mem_alloc(h, (size_t)(size > 0 ? size : 1), 1);
        if (data == NULL) {
            out = tz_fail(burrow_err_out_of_memory);
            goto done;
        }
        if (!tz_preadn(fd, data, size, off + 30 + namelen + xlen)) {
            mem_free(h, data, (size_t)(size > 0 ? size : 1), 1);
            goto done;
        }
        out = (TzFile){data, size > 0 ? size : 1, BURROW_NO_ERROR, false};
        out.len = size;
        if (size == 0) {
            mem_free(h, data, 1, 1);
            out.p = NULL;
        }
        goto done;
    }
    out = tz_not_found();

done:
    if (dir != NULL)
        mem_free(h, dir, (size_t)(dsize > 0 ? dsize : 1), 1);
    (void)pal_close(fd, NULL);
    return out;
}

static TzFile tz_load_from_dir_or_zip(Str dir, Str name) {
    if (dir.len > 4 && memcmp(dir.p + dir.len - 4, ".zip", 4) == 0)
        return tz_load_from_zip(dir, name);
    if (dir.len == 0)
        return tz_read_file(name);
    size_t size;
    char *p = tz_path(dir, name, &size);
    if (p == NULL)
        return tz_fail(burrow_err_out_of_memory);
    TzFile f = tz_read_file((Str){(const Byte *)p, (Int)size - 1});
    mem_free(heap_allocator(), p, size, 1);
    return f;
}

#if defined(BURROW_OS_WINDOWS)
/* Windows has no zoneinfo directory, and Go reads the registry instead. */
static const char *const tz_platform_sources[1] = {NULL};
enum { TZ_NPLATFORM = 0 };
#else
static const char *const tz_platform_sources[4] = {
    "/usr/share/zoneinfo/",
    "/usr/share/lib/zoneinfo/",
    "/usr/lib/locale/TZ/",
    "/etc/zoneinfo",
};
enum { TZ_NPLATFORM = 4 };
#endif

/* Go's loadLocation: name from each source in turn, the location from the
 * first that has it, and otherwise the first error that was not a missing
 * file. The location comes from the heap and is never freed. */
static TimeLocation *tz_load_sources(Str name, const char *const *sources, Int n,
                                     Error *err) {
    Error first = BURROW_NO_ERROR;
    for (Int i = 0; i < n; i++) {
        TzFile f = tz_load_from_dir_or_zip(tz_cstr(sources[i]), name);
        Error e = f.err;
        bool enoent = f.enoent;
        if (BURROW_OK(f.err)) {
            TimeLocation *z = time_load_location_from_tz_data(
                heap_allocator(), name, (Slice){f.p, f.len, f.len, TYPE_BYTE}, &e);
            tz_file_free(&f);
            if (z != NULL) {
                z->a = NULL;
                return z;
            }
        }
        if (BURROW_OK(first) && !enoent)
            first = e;
    }
    if (BURROW_FAILED(first)) {
        BURROW_OUT(err, first);
        return NULL;
    }
    BURROW_OUT(err, tz_err(tz_concat(BURROW_S("unknown time zone "), name)));
    return NULL;
}

/* The value of the environment variable key, and whether it is set at all,
 * which for TZ is not the same thing as set to nothing. */
static bool tz_getenv(const char *key, Str *val) {
    size_t kn = strlen(key);
    for (const char *const *env = pal_environ(); env != NULL && *env != NULL; env++) {
        if (strncmp(*env, key, kn) == 0 && (*env)[kn] == '=') {
            *val = tz_cstr(*env + kn + 1);
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------ the cache */

static SyncMutex tz_cache_mu;
static TimeLocation *tz_cache;

static bool tz_contains_dot_dot(Str s) {
    for (Int i = 0; i + 1 < s.len; i++) {
        if (s.p[i] == '.' && s.p[i + 1] == '.')
            return true;
    }
    return false;
}

/* Everything LoadLocation does after the checks on the name. */
static TimeLocation *tz_load(Str name, Error *err) {
    Error first = BURROW_NO_ERROR;
    Str zoneinfo;
    if (tz_getenv("ZONEINFO", &zoneinfo) && zoneinfo.len > 0) {
        TzFile f = tz_load_from_dir_or_zip(zoneinfo, name);
        if (BURROW_OK(f.err)) {
            TimeLocation *z = time_load_location_from_tz_data(
                heap_allocator(), name, (Slice){f.p, f.len, f.len, TYPE_BYTE}, NULL);
            tz_file_free(&f);
            if (z != NULL) {
                z->a = NULL;
                return z;
            }
            /* Go sets its first error here from the read, which succeeded,
             * so bad data under $ZONEINFO is passed over without a word. */
        } else if (!f.enoent) {
            first = f.err;
        }
    }

    Error e = BURROW_NO_ERROR;
    TimeLocation *z = tz_load_sources(name, tz_platform_sources, TZ_NPLATFORM, &e);
    if (z != NULL)
        return z;
    BURROW_OUT(err, BURROW_FAILED(first) ? first : e);
    return NULL;
}

TimeLocation *time_load_location(Str name, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (name.len == 0 || str_eq(name, BURROW_S("UTC")))
        return time_utc_loc;
    if (str_eq(name, BURROW_S("Local")))
        return time_local_loc;
    if (tz_contains_dot_dot(name) || name.p[0] == '/' || name.p[0] == '\\') {
        /* No valid IANA Time Zone name contains a single dot, much less
         * dot dot. Likewise, none begin with a slash. */
        BURROW_OUT(err, tz_err(BURROW_S("time: invalid location name")));
        return NULL;
    }

    sync_mutex_lock(&tz_cache_mu);
    for (TimeLocation *l = tz_cache; l != NULL; l = l->next) {
        if (str_eq(l->name, name)) {
            sync_mutex_unlock(&tz_cache_mu);
            return l;
        }
    }
    sync_mutex_unlock(&tz_cache_mu);

    TimeLocation *z = tz_load(name, err);
    if (z == NULL)
        return NULL;

    /* Two threads can load one name at once. The second to get here takes
     * the first one's and its own is kept too, which costs one location
     * once and saves holding the lock across the file system. */
    sync_mutex_lock(&tz_cache_mu);
    for (TimeLocation *l = tz_cache; l != NULL; l = l->next) {
        if (str_eq(l->name, name)) {
            z->next = NULL;
            sync_mutex_unlock(&tz_cache_mu);
            return l;
        }
    }
    z->next = tz_cache;
    tz_cache = z;
    sync_mutex_unlock(&tz_cache_mu);
    return z;
}

/* ------------------------------------------------------------------ Local */

/* Local becomes a copy of z under another name. z's zones and transitions
 * stay where they are, and so does z, which nothing frees. */
static void tz_set_local(TimeLocation *z, Str name) {
    tz_local_loc = *z;
    if (z->cache_zone == &z->cache_own)
        tz_local_loc.cache_zone = &tz_local_loc.cache_own;
    tz_local_loc.name = name;
    tz_local_loc.next = NULL;
    tz_local_loc.a = NULL;
}

/* The Local that force_local replaces. Its zones still live in the block
 * tz_load_sources allocated, and this keeps that block reachable. */
static TimeLocation tz_displaced_loc;

void burrow__time_force_local(TimeLocation *l) {
    (void)burrow__time_loc_get(time_local_loc);
    if (tz_displaced_loc.name.p == NULL)
        tz_displaced_loc = tz_local_loc;
    tz_set_local(l, BURROW_S("Local"));
}

#if defined(BURROW_OS_WINDOWS)

static void tz_init_local(void *arg) {
    (void)arg;
    tz_local_loc.name = BURROW_S("UTC");
}

#else

static void tz_init_local(void *arg) {
    (void)arg;
    /* Consult $TZ to find the time zone to use. No $TZ means use the system
     * default /etc/localtime. $TZ="" means use UTC. $TZ="foo" or $TZ=":foo"
     * means use the file foo, looked for in the zoneinfo directories. */
    Str tz;
    bool ok = tz_getenv("TZ", &tz);
    if (!ok) {
        static const char *const etc[1] = {"/etc"};
        TimeLocation *z = tz_load_sources(BURROW_S("localtime"), etc, 1, NULL);
        if (z != NULL) {
            tz_set_local(z, BURROW_S("Local"));
            return;
        }
    } else if (tz.len > 0) {
        if (tz.p[0] == ':')
            tz = tz_rest(tz, 1);
        if (tz.len > 0 && tz.p[0] == '/') {
            static const char *const none[1] = {""};
            TimeLocation *z = tz_load_sources(tz, none, 1, NULL);
            if (z != NULL) {
                if (str_eq(tz, BURROW_S("/etc/localtime")))
                    tz_set_local(z, BURROW_S("Local"));
                else
                    tz_set_local(z, z->name);
                return;
            }
        } else if (tz.len > 0 && !str_eq(tz, BURROW_S("UTC"))) {
            TimeLocation *z =
                tz_load_sources(tz, tz_platform_sources, TZ_NPLATFORM, NULL);
            if (z != NULL) {
                tz_set_local(z, z->name);
                return;
            }
        }
    }
    /* Fall back to UTC. */
    tz_local_loc.name = BURROW_S("UTC");
}

#endif
