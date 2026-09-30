/* Format and Parse: Go's format.go and format_rfc3339.go, and the text and
 * JSON encodings of a Time that are built on them.
 *
 * A layout is a sample of the reference time, Mon Jan 2 15:04:05 MST 2006,
 * and tf_next_chunk is Go's nextStdChunk, which finds the next piece of it.
 * Everything else follows Go's code closely, down to what it accepts that it
 * arguably should not, such as a sign in front of a two digit year, because a
 * program that parses its input with Go today should get the same answer here.
 *
 * Formatting writes through a TfOut, which counts every byte and stores the
 * ones that fit, so the common case is one pass into a buffer on the stack and
 * a long result is a second pass into memory of the right size.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/time.h"

#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/encoding.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "time_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ----------------------------------------------------------------- chunks */

/* Go's std values. The low byte numbers the element, the flags above it say
 * which parts of the date it needs, and a fractional second keeps its digit
 * count and its separator in the high bits. */
enum {
    TF_NEED_DATE = 1 << 8,
    TF_NEED_YDAY = 1 << 9,
    TF_NEED_CLOCK = 1 << 10,
    TF_ARG_SHIFT = 16,
    TF_SEPARATOR_SHIFT = 28,
    TF_MASK = (1 << TF_ARG_SHIFT) - 1,

    TF_LONG_MONTH = 1 + TF_NEED_DATE,      /* "January" */
    TF_MONTH = 2 + TF_NEED_DATE,           /* "Jan" */
    TF_NUM_MONTH = 3 + TF_NEED_DATE,       /* "1" */
    TF_ZERO_MONTH = 4 + TF_NEED_DATE,      /* "01" */
    TF_LONG_WEEK_DAY = 5 + TF_NEED_DATE,   /* "Monday" */
    TF_WEEK_DAY = 6 + TF_NEED_DATE,        /* "Mon" */
    TF_DAY = 7 + TF_NEED_DATE,             /* "2" */
    TF_UNDER_DAY = 8 + TF_NEED_DATE,       /* "_2" */
    TF_ZERO_DAY = 9 + TF_NEED_DATE,        /* "02" */
    TF_UNDER_YEAR_DAY = 10 + TF_NEED_YDAY, /* "__2" */
    TF_ZERO_YEAR_DAY = 11 + TF_NEED_YDAY,  /* "002" */
    TF_HOUR = 12 + TF_NEED_CLOCK,          /* "15" */
    TF_HOUR12 = 13 + TF_NEED_CLOCK,        /* "3" */
    TF_ZERO_HOUR12 = 14 + TF_NEED_CLOCK,   /* "03" */
    TF_MINUTE = 15 + TF_NEED_CLOCK,        /* "4" */
    TF_ZERO_MINUTE = 16 + TF_NEED_CLOCK,   /* "04" */
    TF_SECOND = 17 + TF_NEED_CLOCK,        /* "5" */
    TF_ZERO_SECOND = 18 + TF_NEED_CLOCK,   /* "05" */
    TF_LONG_YEAR = 19 + TF_NEED_DATE,      /* "2006" */
    TF_YEAR = 20 + TF_NEED_DATE,           /* "06" */
    TF_PM = 21 + TF_NEED_CLOCK,            /* "PM" */
    TF_PM_LOWER = 22 + TF_NEED_CLOCK,      /* "pm" */
    TF_TZ = 23,                            /* "MST" */
    TF_ISO8601_TZ = 24,                    /* "Z0700", Z for UTC */
    TF_ISO8601_SECONDS_TZ = 25,            /* "Z070000" */
    TF_ISO8601_SHORT_TZ = 26,              /* "Z07" */
    TF_ISO8601_COLON_TZ = 27,              /* "Z07:00", Z for UTC */
    TF_ISO8601_COLON_SECONDS_TZ = 28,      /* "Z07:00:00" */
    TF_NUM_TZ = 29,                        /* "-0700", always numeric */
    TF_NUM_SECONDS_TZ = 30,                /* "-070000" */
    TF_NUM_SHORT_TZ = 31,                  /* "-07", always numeric */
    TF_NUM_COLON_TZ = 32,                  /* "-07:00", always numeric */
    TF_NUM_COLON_SECONDS_TZ = 33,          /* "-07:00:00" */
    TF_FRAC_SECOND0 = 34,                  /* ".0", ".00", trailing zeros kept */
    TF_FRAC_SECOND9 = 35,                  /* ".9", ".99", trailing zeros dropped */
};

/* "01" to "06". */
static const int tf_std0x[] = {TF_ZERO_MONTH,  TF_ZERO_DAY,    TF_ZERO_HOUR12,
                               TF_ZERO_MINUTE, TF_ZERO_SECOND, TF_YEAR};

static bool tf_has(Str s, Int i, const char *lit) {
    size_t n = strlen(lit);
    return s.len >= i + (Int)n && memcmp(s.p + i, lit, n) == 0;
}

static bool tf_is_digit(Str s, Int i) {
    return i < s.len && '0' <= s.p[i] && s.p[i] <= '9';
}

/* Whether s starts with a lower case letter, so that "Month" is not "Mon". */
static bool tf_starts_lower(Str s, Int i) {
    return i < s.len && 'a' <= s.p[i] && s.p[i] <= 'z';
}

static int tf_frac(int code, Int n, Byte c) {
    int std = code | (int)(((uint32_t)n & 0xfffU) << TF_ARG_SHIFT);
    if (c != '.')
        std |= 1 << TF_SEPARATOR_SHIFT;
    return std;
}

static Int tf_digits_len(int std) {
    return (Int)(((uint32_t)std >> TF_ARG_SHIFT) & 0xfffU);
}

static Byte tf_separator(int std) {
    return ((uint32_t)std >> TF_SEPARATOR_SHIFT) == 0 ? '.' : ',';
}

/* Go's nextStdChunk: the text before the next element, the element, and the
 * text after it. No element left gives 0 and the whole layout as the prefix. */
static int tf_next_chunk(Str layout, Str *prefix, Str *suffix) {
    const Byte *l = layout.p;
    Int n = layout.len;
#define TF_CHUNK(std, skip)                                                            \
    do {                                                                               \
        *prefix = str_from_bytes(l, i);                                                \
        *suffix = str_from_bytes(l + i + (skip), n - i - (skip));                      \
        return (std);                                                                  \
    } while (0)
    for (Int i = 0; i < n; i++) {
        switch (l[i]) {
        case 'J': /* January, Jan */
            if (tf_has(layout, i, "Jan")) {
                if (tf_has(layout, i, "January"))
                    TF_CHUNK(TF_LONG_MONTH, 7);
                if (!tf_starts_lower(layout, i + 3))
                    TF_CHUNK(TF_MONTH, 3);
            }
            break;
        case 'M': /* Monday, Mon, MST */
            if (n >= i + 3) {
                if (tf_has(layout, i, "Mon")) {
                    if (tf_has(layout, i, "Monday"))
                        TF_CHUNK(TF_LONG_WEEK_DAY, 6);
                    if (!tf_starts_lower(layout, i + 3))
                        TF_CHUNK(TF_WEEK_DAY, 3);
                }
                if (tf_has(layout, i, "MST"))
                    TF_CHUNK(TF_TZ, 3);
            }
            break;
        case '0': /* 01, 02, 03, 04, 05, 06, 002 */
            if (n >= i + 2 && '1' <= l[i + 1] && l[i + 1] <= '6')
                TF_CHUNK(tf_std0x[l[i + 1] - '1'], 2);
            if (n >= i + 3 && l[i + 1] == '0' && l[i + 2] == '2')
                TF_CHUNK(TF_ZERO_YEAR_DAY, 3);
            break;
        case '1': /* 15, 1 */
            if (n >= i + 2 && l[i + 1] == '5')
                TF_CHUNK(TF_HOUR, 2);
            TF_CHUNK(TF_NUM_MONTH, 1);
        case '2': /* 2006, 2 */
            if (tf_has(layout, i, "2006"))
                TF_CHUNK(TF_LONG_YEAR, 4);
            TF_CHUNK(TF_DAY, 1);
        case '_': /* _2, _2006, __2 */
            if (n >= i + 2 && l[i + 1] == '2') {
                /* _2006 is a literal _ and then the long year. */
                if (tf_has(layout, i + 1, "2006")) {
                    *prefix = str_from_bytes(l, i + 1);
                    *suffix = str_from_bytes(l + i + 5, n - i - 5);
                    return TF_LONG_YEAR;
                }
                TF_CHUNK(TF_UNDER_DAY, 2);
            }
            if (n >= i + 3 && l[i + 1] == '_' && l[i + 2] == '2')
                TF_CHUNK(TF_UNDER_YEAR_DAY, 3);
            break;
        case '3':
            TF_CHUNK(TF_HOUR12, 1);
        case '4':
            TF_CHUNK(TF_MINUTE, 1);
        case '5':
            TF_CHUNK(TF_SECOND, 1);
        case 'P': /* PM */
            if (n >= i + 2 && l[i + 1] == 'M')
                TF_CHUNK(TF_PM, 2);
            break;
        case 'p': /* pm */
            if (n >= i + 2 && l[i + 1] == 'm')
                TF_CHUNK(TF_PM_LOWER, 2);
            break;
        case '-': /* -070000, -07:00:00, -0700, -07:00, -07 */
            if (tf_has(layout, i, "-070000"))
                TF_CHUNK(TF_NUM_SECONDS_TZ, 7);
            if (tf_has(layout, i, "-07:00:00"))
                TF_CHUNK(TF_NUM_COLON_SECONDS_TZ, 9);
            if (tf_has(layout, i, "-0700"))
                TF_CHUNK(TF_NUM_TZ, 5);
            if (tf_has(layout, i, "-07:00"))
                TF_CHUNK(TF_NUM_COLON_TZ, 6);
            if (tf_has(layout, i, "-07"))
                TF_CHUNK(TF_NUM_SHORT_TZ, 3);
            break;
        case 'Z': /* Z070000, Z07:00:00, Z0700, Z07:00, Z07 */
            if (tf_has(layout, i, "Z070000"))
                TF_CHUNK(TF_ISO8601_SECONDS_TZ, 7);
            if (tf_has(layout, i, "Z07:00:00"))
                TF_CHUNK(TF_ISO8601_COLON_SECONDS_TZ, 9);
            if (tf_has(layout, i, "Z0700"))
                TF_CHUNK(TF_ISO8601_TZ, 5);
            if (tf_has(layout, i, "Z07:00"))
                TF_CHUNK(TF_ISO8601_COLON_TZ, 6);
            if (tf_has(layout, i, "Z07"))
                TF_CHUNK(TF_ISO8601_SHORT_TZ, 3);
            break;
        case '.':
        case ',': /* .000, ,000, .999 or ,999: a fractional second */
            if (i + 1 < n && (l[i + 1] == '0' || l[i + 1] == '9')) {
                Byte ch = l[i + 1];
                Int j = i + 1;
                while (j < n && l[j] == ch)
                    j++;
                /* Only a run of digits that ends here is a fraction. */
                if (!tf_is_digit(layout, j)) {
                    int code = ch == '9' ? TF_FRAC_SECOND9 : TF_FRAC_SECOND0;
                    int std = tf_frac(code, j - (i + 1), l[i]);
                    *prefix = str_from_bytes(l, i);
                    *suffix = str_from_bytes(l + j, n - j);
                    return std;
                }
            }
            break;
        default:
            break;
        }
    }
#undef TF_CHUNK
    *prefix = layout;
    *suffix = str_from_bytes(l + n, 0);
    return 0;
}

static const char *const tf_long_day_names[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};

static const char *const tf_short_day_names[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
};

static const char *const tf_short_month_names[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
};

static const char *const tf_long_month_names[] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December",
};

/* ----------------------------------------------------------------- output */

/* Where formatting goes: p has room for cap bytes, and n counts every byte
 * written, including the ones that did not fit. */
typedef struct TfOut {
    Byte *p;
    Int n;
    Int cap;
} TfOut;

static void tf_put(TfOut *o, const void *s, Int len) {
    if (o->p != NULL && len > 0 && o->n + len <= o->cap)
        memcpy(o->p + o->n, s, (size_t)len);
    o->n += len;
}

static void tf_putc(TfOut *o, Byte c) {
    if (o->n < o->cap)
        o->p[o->n] = c;
    o->n++;
}

static void tf_puts(TfOut *o, const char *s) {
    tf_put(o, s, (Int)strlen(s));
}

/* Go's appendInt: x in decimal, padded with zeros to width digits, the sign
 * not counted. */
static void tf_int(TfOut *o, Int x, Int width) {
    uint64_t u = (uint64_t)x;
    if (x < 0) {
        tf_putc(o, '-');
        u = -u;
    }
    Byte tmp[24];
    Int i = (Int)sizeof tmp;
    do {
        tmp[--i] = (Byte)('0' + u % 10);
        u /= 10;
    } while (u > 0);
    for (Int pad = width - ((Int)sizeof tmp - i); pad > 0; pad--)
        tf_putc(o, '0');
    tf_put(o, tmp + i, (Int)sizeof tmp - i);
}

/* Go's appendNano: the fraction of a second, with the digit count and the
 * separator packed into std. */
static void tf_nano(TfOut *o, Int nanosec, int std) {
    bool trim = (std & TF_MASK) == TF_FRAC_SECOND9;
    Int n = tf_digits_len(std);
    if (trim && (n == 0 || nanosec == 0))
        return;
    Byte buf[10];
    buf[0] = tf_separator(std);
    uint64_t u = (uint64_t)nanosec;
    for (int i = 9; i >= 1; i--) {
        buf[i] = (Byte)('0' + u % 10);
        u /= 10;
    }
    Int len = 1 + (n < 9 ? n : 9);
    if (trim) {
        while (len > 1 && buf[len - 1] == '0')
            len--;
        if (len == 1)
            len = 0;
    }
    tf_put(o, buf, len);
}

/* Go's Time.locabs: the zone's name and offset at t, and t as absolute
 * seconds in that zone. */
static uint64_t tf_locabs(Time t, Str *name, Int *offset) {
    TimeLocation *l = burrow__time_loc_get(t.loc);
    int64_t sec = burrow__time_unix_sec(&t);
    if (l != burrow__time_utc()) {
        if (l->cache_zone != NULL && l->cache_start <= sec && sec < l->cache_end) {
            *name = l->cache_zone->name;
            *offset = l->cache_zone->offset;
        } else {
            TzLookup z = burrow__time_lookup(l, sec);
            *name = z.name;
            *offset = z.offset;
        }
        sec = (int64_t)((uint64_t)sec + (uint64_t)*offset);
    } else {
        *name = BURROW_S("UTC");
        *offset = 0;
    }
    return (uint64_t)sec + (uint64_t)TIME_UNIX_TO_ABSOLUTE;
}

static void tf_clock(uint64_t abs, Int *hour, Int *min, Int *sec) {
    Int s = (Int)(abs % TZ_SECONDS_PER_DAY);
    *hour = s / TZ_SECONDS_PER_HOUR;
    s -= *hour * TZ_SECONDS_PER_HOUR;
    *min = s / TZ_SECONDS_PER_MINUTE;
    *sec = s - *min * TZ_SECONDS_PER_MINUTE;
}

static bool tf_is_iso_tz(int std) {
    return std == TF_ISO8601_TZ || std == TF_ISO8601_COLON_TZ ||
           std == TF_ISO8601_SECONDS_TZ || std == TF_ISO8601_SHORT_TZ ||
           std == TF_ISO8601_COLON_SECONDS_TZ;
}

/* Go's Time.appendFormat. */
static void tf_format(TfOut *o, Time t, Str layout) {
    Str name;
    Int offset;
    uint64_t abs = tf_locabs(t, &name, &offset);
    uint64_t days = abs / TZ_SECONDS_PER_DAY;

    /* Worked out when the first element that needs them comes along. Go
     * tests year < 0 for the date, which works the date out again for every
     * element of a year before 1 and gets the same answer each time. */
    bool have_date = false;
    bool have_yday = false;
    bool have_clock = false;
    Int year = 0;
    TimeMonth month = TIME_JANUARY;
    Int day = 1;
    Int yday = 1;
    Int hour = 0;
    Int min = 0;
    Int sec = 0;

    while (layout.len > 0) {
        Str prefix;
        Str suffix;
        int std = tf_next_chunk(layout, &prefix, &suffix);
        tf_put(o, prefix.p, prefix.len);
        if (std == 0)
            break;
        layout = suffix;

        if (!have_date && (std & TF_NEED_DATE) != 0) {
            TimeYmd d = burrow__time_abs_date(days);
            year = d.year;
            month = d.month;
            day = d.day;
            have_date = true;
        }
        if (!have_yday && (std & TF_NEED_YDAY) != 0) {
            Int y;
            burrow__time_abs_year_yday(days, &y, &yday);
            have_yday = true;
        }
        if (!have_clock && (std & TF_NEED_CLOCK) != 0) {
            tf_clock(abs, &hour, &min, &sec);
            have_clock = true;
        }

        switch (std & TF_MASK) {
        case TF_YEAR: {
            Int y = year < 0 ? -year : year;
            tf_int(o, y % 100, 2);
            break;
        }
        case TF_LONG_YEAR:
            tf_int(o, year, 4);
            break;
        case TF_MONTH:
            tf_puts(o, tf_short_month_names[month - 1]);
            break;
        case TF_LONG_MONTH:
            tf_puts(o, tf_long_month_names[month - 1]);
            break;
        case TF_NUM_MONTH:
            tf_int(o, month, 0);
            break;
        case TF_ZERO_MONTH:
            tf_int(o, month, 2);
            break;
        case TF_WEEK_DAY:
            tf_puts(o, tf_short_day_names[(days + (uint64_t)TIME_WEDNESDAY) % 7]);
            break;
        case TF_LONG_WEEK_DAY:
            tf_puts(o, tf_long_day_names[(days + (uint64_t)TIME_WEDNESDAY) % 7]);
            break;
        case TF_DAY:
            tf_int(o, day, 0);
            break;
        case TF_UNDER_DAY:
            if (day < 10)
                tf_putc(o, ' ');
            tf_int(o, day, 0);
            break;
        case TF_ZERO_DAY:
            tf_int(o, day, 2);
            break;
        case TF_UNDER_YEAR_DAY:
            if (yday < 100) {
                tf_putc(o, ' ');
                if (yday < 10)
                    tf_putc(o, ' ');
            }
            tf_int(o, yday, 0);
            break;
        case TF_ZERO_YEAR_DAY:
            tf_int(o, yday, 3);
            break;
        case TF_HOUR:
            tf_int(o, hour, 2);
            break;
        case TF_HOUR12:
        case TF_ZERO_HOUR12: {
            /* Noon is 12PM and midnight is 12AM. */
            Int hr = hour % 12;
            if (hr == 0)
                hr = 12;
            tf_int(o, hr, (std & TF_MASK) == TF_ZERO_HOUR12 ? 2 : 0);
            break;
        }
        case TF_MINUTE:
            tf_int(o, min, 0);
            break;
        case TF_ZERO_MINUTE:
            tf_int(o, min, 2);
            break;
        case TF_SECOND:
            tf_int(o, sec, 0);
            break;
        case TF_ZERO_SECOND:
            tf_int(o, sec, 2);
            break;
        case TF_PM:
            tf_puts(o, hour >= 12 ? "PM" : "AM");
            break;
        case TF_PM_LOWER:
            tf_puts(o, hour >= 12 ? "pm" : "am");
            break;
        case TF_ISO8601_TZ:
        case TF_ISO8601_COLON_TZ:
        case TF_ISO8601_SECONDS_TZ:
        case TF_ISO8601_SHORT_TZ:
        case TF_ISO8601_COLON_SECONDS_TZ:
        case TF_NUM_TZ:
        case TF_NUM_COLON_TZ:
        case TF_NUM_SECONDS_TZ:
        case TF_NUM_SHORT_TZ:
        case TF_NUM_COLON_SECONDS_TZ: {
            /* The Z forms print Z for UTC, which is Go's way of saying the
             * time zone as ISO 8601 writes it. */
            if (offset == 0 && tf_is_iso_tz(std)) {
                tf_putc(o, 'Z');
                break;
            }
            Int zone = offset / 60;
            Int absoffset = offset;
            if (zone < 0) {
                tf_putc(o, '-');
                zone = -zone;
                absoffset = -absoffset;
            } else {
                tf_putc(o, '+');
            }
            tf_int(o, zone / 60, 2);
            if (std == TF_ISO8601_COLON_TZ || std == TF_NUM_COLON_TZ ||
                std == TF_ISO8601_COLON_SECONDS_TZ || std == TF_NUM_COLON_SECONDS_TZ)
                tf_putc(o, ':');
            if (std != TF_NUM_SHORT_TZ && std != TF_ISO8601_SHORT_TZ)
                tf_int(o, zone % 60, 2);
            if (std == TF_ISO8601_SECONDS_TZ || std == TF_NUM_SECONDS_TZ ||
                std == TF_NUM_COLON_SECONDS_TZ || std == TF_ISO8601_COLON_SECONDS_TZ) {
                if (std == TF_NUM_COLON_SECONDS_TZ ||
                    std == TF_ISO8601_COLON_SECONDS_TZ)
                    tf_putc(o, ':');
                tf_int(o, absoffset % 60, 2);
            }
            break;
        }
        case TF_TZ: {
            if (name.len > 0) {
                tf_put(o, name.p, name.len);
                break;
            }
            /* A zone with no name still has to print as something, and Go
             * picks -0700. */
            Int zone = offset / 60;
            if (zone < 0) {
                tf_putc(o, '-');
                zone = -zone;
            } else {
                tf_putc(o, '+');
            }
            tf_int(o, zone / 60, 2);
            tf_int(o, zone % 60, 2);
            break;
        }
        case TF_FRAC_SECOND0:
        case TF_FRAC_SECOND9:
            tf_nano(o, time_nanosecond(t), std);
            break;
        default:
            break;
        }
    }
}

/* Go's appendFormatRFC3339, which is what the RFC3339 layouts take instead of
 * the general loop, because more than half of all formatting uses them. */
static void tf_format_rfc3339(TfOut *o, Time t, bool nanos) {
    Str name;
    Int offset;
    uint64_t abs = tf_locabs(t, &name, &offset);
    TimeYmd d = burrow__time_abs_date(abs / TZ_SECONDS_PER_DAY);
    tf_int(o, d.year, 4);
    tf_putc(o, '-');
    tf_int(o, d.month, 2);
    tf_putc(o, '-');
    tf_int(o, d.day, 2);
    tf_putc(o, 'T');
    Int hour;
    Int min;
    Int sec;
    tf_clock(abs, &hour, &min, &sec);
    tf_int(o, hour, 2);
    tf_putc(o, ':');
    tf_int(o, min, 2);
    tf_putc(o, ':');
    tf_int(o, sec, 2);
    if (nanos)
        tf_nano(o, time_nanosecond(t), tf_frac(TF_FRAC_SECOND9, 9, '.'));
    if (offset == 0) {
        tf_putc(o, 'Z');
        return;
    }
    Int zone = offset / 60;
    if (zone < 0) {
        tf_putc(o, '-');
        zone = -zone;
    } else {
        tf_putc(o, '+');
    }
    tf_int(o, zone / 60, 2);
    tf_putc(o, ':');
    tf_int(o, zone % 60, 2);
}

/* Go's Time.AppendFormat, without the append. */
static void tf_append_format(TfOut *o, Time t, Str layout) {
    if (str_eq(layout, TIME_RFC3339))
        tf_format_rfc3339(o, t, false);
    else if (str_eq(layout, TIME_RFC3339_NANO))
        tf_format_rfc3339(o, t, true);
    else
        tf_format(o, t, layout);
}

enum { TF_STACK = 128 };

Str time_format(Time t, Alloc *a, Str layout) {
    Byte small[TF_STACK];
    TfOut o = {small, 0, TF_STACK};
    tf_append_format(&o, t, layout);
    if (o.n <= o.cap)
        return str_clone(a, str_from_bytes(small, o.n));
    Byte *p = mem_alloc_nozero(a, (size_t)o.n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    TfOut big = {p, 0, o.n};
    tf_append_format(&big, t, layout);
    return str_from_bytes(p, big.n);
}

/* Appends what fill writes, into b's spare room when there is enough of it
 * and through a buffer on the stack or from a when there is not. */
typedef void (*TfFill)(TfOut *o, Time t, Str layout);

static Slice tf_append(Alloc *a, Slice b, Time t, Str layout, TfFill fill) {
    if (b.elem == NULL)
        b = slice_nil(TYPE_BYTE);
    Byte small[TF_STACK];
    TfOut o = {small, 0, TF_STACK};
    fill(&o, t, layout);
    if (o.n <= o.cap)
        return slice_append(a, b, small, o.n);
    Byte *p = mem_alloc_nozero(a, (size_t)o.n, 1);
    if (p == NULL)
        return b;
    TfOut big = {p, 0, o.n};
    fill(&big, t, layout);
    Slice out = slice_append(a, b, p, big.n);
    mem_free(a, p, (size_t)big.n, 1);
    return out;
}

Slice time_append_format(Time t, Alloc *a, Slice b, Str layout) {
    return tf_append(a, b, t, layout, tf_append_format);
}

/* Go's Time.String: the layout below, and the monotonic reading as
 * m=±seconds when t has one. */
static void tf_string(TfOut *o, Time t, Str layout) {
    tf_format(o, t, layout);
    if ((t.wall & ((uint64_t)1 << 63)) == 0)
        return;
    uint64_t m2 = (uint64_t)t.ext;
    Byte sign = '+';
    if (t.ext < 0) {
        sign = '-';
        m2 = -m2;
    }
    uint64_t m1 = m2 / 1000000000;
    m2 %= 1000000000;
    uint64_t m0 = m1 / 1000000000;
    m1 %= 1000000000;
    tf_puts(o, " m=");
    tf_putc(o, sign);
    Int wid = 0;
    if (m0 != 0) {
        tf_int(o, (Int)m0, 0);
        wid = 9;
    }
    tf_int(o, (Int)m1, wid);
    tf_putc(o, '.');
    tf_int(o, (Int)m2, 9);
}

Str time_string(Time t, Alloc *a) {
    Str layout = BURROW_S("2006-01-02 15:04:05.999999999 -0700 MST");
    Byte small[TF_STACK];
    TfOut o = {small, 0, TF_STACK};
    tf_string(&o, t, layout);
    if (o.n <= o.cap)
        return str_clone(a, str_from_bytes(small, o.n));
    Byte *p = mem_alloc_nozero(a, (size_t)o.n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    TfOut big = {p, 0, o.n};
    tf_string(&big, t, layout);
    return str_from_bytes(p, big.n);
}

/* Go's Time.GoString. */
static void tf_go_string(TfOut *o, Time t, Str unused) {
    (void)unused;
    Str name;
    Int offset;
    uint64_t abs = tf_locabs(t, &name, &offset);
    TimeYmd d = burrow__time_abs_date(abs / TZ_SECONDS_PER_DAY);
    Int hour;
    Int min;
    Int sec;
    tf_clock(abs, &hour, &min, &sec);

    tf_puts(o, "time.Date(");
    tf_int(o, d.year, 0);
    if (TIME_JANUARY <= d.month && d.month <= TIME_DECEMBER) {
        tf_puts(o, ", time.");
        tf_puts(o, tf_long_month_names[d.month - 1]);
    } else {
        tf_int(o, d.month, 0);
    }
    tf_puts(o, ", ");
    tf_int(o, d.day, 0);
    tf_puts(o, ", ");
    tf_int(o, hour, 0);
    tf_puts(o, ", ");
    tf_int(o, min, 0);
    tf_puts(o, ", ");
    tf_int(o, sec, 0);
    tf_puts(o, ", ");
    tf_int(o, time_nanosecond(t), 0);
    tf_puts(o, ", ");
    TimeLocation *loc = time_location(t);
    if (loc == time_utc_loc) {
        tf_puts(o, "time.UTC");
    } else if (loc == time_local_loc) {
        tf_puts(o, "time.Local");
    } else {
        /* Go's choice too: not valid Go, and the least bad of the options. */
        tf_puts(o, "time.Location(");
        Str lname = time_location_string(loc);
        Int q = burrow__time_quote(lname, NULL);
        if (o->n + q <= o->cap)
            (void)burrow__time_quote(lname, o->p + o->n);
        o->n += q;
        tf_putc(o, ')');
    }
    tf_putc(o, ')');
}

Str time_go_string(Time t, Alloc *a) {
    Byte small[TF_STACK];
    TfOut o = {small, 0, TF_STACK};
    tf_go_string(&o, t, BURROW_STR_EMPTY);
    if (o.n <= o.cap)
        return str_clone(a, str_from_bytes(small, o.n));
    Byte *p = mem_alloc_nozero(a, (size_t)o.n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    TfOut big = {p, 0, o.n};
    tf_go_string(&big, t, BURROW_STR_EMPTY);
    return str_from_bytes(p, big.n);
}

/* ------------------------------------------------------------- ParseError */

/* The public struct first, so the data pointer of the Error is a pointer to a
 * TimeParseError and errors_as hands it straight back, then the message, which
 * is built once because the message slot can not allocate. */
typedef struct TfErrorBox {
    TimeParseError e;
    Str text;
} TfErrorBox;

static Str tf_error_message(const void *self) {
    return ((const TfErrorBox *)self)->text;
}

static const Type tf_error_desc = {
    {(const Byte *)"ParseError", 10},
    {(const Byte *)"time", 4},
    KIND_STRUCT,
    (uint32_t)sizeof(TimeParseError),
    (uint16_t)_Alignof(TimeParseError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x74706572U, /* "tper" */
    NULL,
};

const Type *const TYPE_TIME_PARSE_ERROR = &tf_error_desc;

static Error tf_error_clone(const void *self, Alloc *a);

static const ErrorVT tf_error_vt = {
    &tf_error_desc, tf_error_message, NULL, NULL, NULL, NULL, tf_error_clone,
};

/* The text of Go's ParseError.Error, written to out when out is not NULL, and
 * its length. */
static Int tf_error_text(const TimeParseError *e, Byte *out) {
    TfOut o = {out, 0, out != NULL ? INT64_MAX : 0};
#define TF_QUOTE(s)                                                                    \
    do {                                                                               \
        o.n += burrow__time_quote((s), out != NULL ? out + o.n : NULL);                \
    } while (0)
    tf_puts(&o, "parsing time ");
    TF_QUOTE(e->value);
    if (e->message.len == 0) {
        tf_puts(&o, " as ");
        TF_QUOTE(e->layout);
        tf_puts(&o, ": cannot parse ");
        TF_QUOTE(e->value_elem);
        tf_puts(&o, " as ");
        TF_QUOTE(e->layout_elem);
    } else {
        tf_put(&o, e->message.p, e->message.len);
    }
#undef TF_QUOTE
    return o.n;
}

/* One allocation holds the box, copies of the five strings and the message. */
static Error tf_error_build(Alloc *a, const TimeParseError *e) {
    Int tlen = tf_error_text(e, NULL);
    size_t size = sizeof(TfErrorBox) + (size_t)e->layout.len + (size_t)e->value.len +
                  (size_t)e->layout_elem.len + (size_t)e->value_elem.len +
                  (size_t)e->message.len + (size_t)tlen;
    TfErrorBox *b = mem_alloc_nozero(a, size, _Alignof(TfErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
#define TF_COPY(f)                                                                     \
    do {                                                                               \
        if (e->f.len > 0)                                                              \
            memcpy(p, e->f.p, (size_t)e->f.len);                                       \
        b->e.f = str_from_bytes(p, e->f.len);                                          \
        p += e->f.len;                                                                 \
    } while (0)
    TF_COPY(layout);
    TF_COPY(value);
    TF_COPY(layout_elem);
    TF_COPY(value_elem);
    TF_COPY(message);
#undef TF_COPY
    (void)tf_error_text(&b->e, p);
    b->text = str_from_bytes(p, tlen);
    return (Error){&tf_error_vt, b};
}

static Error tf_error_clone(const void *self, Alloc *a) {
    return tf_error_build(a, (const TimeParseError *)self);
}

Error time_parse_error_as_error(Alloc *a, const TimeParseError *e) {
    return tf_error_build(a, e);
}

Str time_parse_error_error(Alloc *a, const TimeParseError *e) {
    Int n = tf_error_text(e, NULL);
    Byte *p = mem_alloc_nozero(a, (size_t)n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    (void)tf_error_text(e, p);
    return str_from_bytes(p, n);
}

/* Go's newParseError, with the error in the goroutine's error arena. The
 * message, when there is one, is built here from its parts: a literal and,
 * for the extra text case, the quoted rest of the value. */
static Error tf_parse_error(Str alayout, Str avalue, Str layout_elem, Str value_elem,
                            const char *msg, const Str *quoted) {
    Byte small[256];
    Byte *mp = small;
    Int mlen = 0;
    Alloc *ea = error_allocator();
    if (msg != NULL) {
        Int ml = (Int)strlen(msg);
        mlen = ml + (quoted != NULL ? burrow__time_quote(*quoted, NULL) : 0);
        if (mlen > (Int)sizeof small) {
            mp = mem_alloc_nozero(ea, (size_t)mlen, 1);
            if (mp == NULL)
                return burrow_err_out_of_memory;
        }
        memcpy(mp, msg, (size_t)ml);
        if (quoted != NULL)
            (void)burrow__time_quote(*quoted, mp + ml);
    }
    TimeParseError e = {alayout, avalue, layout_elem, value_elem,
                        str_from_bytes(mp, mlen)};
    Error err = tf_error_build(ea, &e);
    if (mp != small)
        mem_free(ea, mp, (size_t)mlen, 1);
    return err;
}

/* ------------------------------------------------------------------ Parse */

/* Go's match: s1 and s2, the same length, equal but for ASCII case. */
static bool tf_match(const Byte *s1, const char *s2, Int n) {
    for (Int i = 0; i < n; i++) {
        Byte c1 = s1[i];
        Byte c2 = (Byte)s2[i];
        if (c1 != c2) {
            c1 |= 'a' - 'A';
            c2 |= 'a' - 'A';
            if (c1 != c2 || c1 < 'a' || c1 > 'z')
                return false;
        }
    }
    return true;
}

/* Go's lookup: which of the names val starts with, and val past it. */
static bool tf_lookup(const char *const *tab, Int ntab, Str *val, Int *idx) {
    for (Int i = 0; i < ntab; i++) {
        Int n = (Int)strlen(tab[i]);
        if (val->len >= n && tf_match(val->p, tab[i], n)) {
            *idx = i;
            val->p += n;
            val->len -= n;
            return true;
        }
    }
    *idx = -1;
    return false;
}

/* Go's leadingInt. */
static bool tf_leading_int(Str s, uint64_t *x, Str *rem) {
    Int i = 0;
    *x = 0;
    for (; i < s.len; i++) {
        Byte c = s.p[i];
        if (c < '0' || c > '9')
            break;
        if (*x > ((uint64_t)1 << 63) / 10)
            return false;
        *x = *x * 10 + (uint64_t)(c - '0');
        if (*x > (uint64_t)1 << 63)
            return false;
    }
    *rem = str_from_bytes(s.p + i, s.len - i);
    return true;
}

/* Go's atoi, which takes a sign. */
static bool tf_atoi(Str s, Int *x) {
    bool neg = false;
    if (s.len > 0 && (s.p[0] == '-' || s.p[0] == '+')) {
        neg = s.p[0] == '-';
        s = str_from_bytes(s.p + 1, s.len - 1);
    }
    uint64_t q;
    Str rem;
    if (!tf_leading_int(s, &q, &rem) || rem.len > 0) {
        *x = 0;
        return false;
    }
    *x = neg ? -(Int)q : (Int)q;
    return true;
}

/* Go's getnum: one or two digits, two when fixed. */
static bool tf_getnum(Str *s, bool fixed, Int *x) {
    *x = 0;
    if (!tf_is_digit(*s, 0))
        return false;
    if (!tf_is_digit(*s, 1)) {
        if (fixed)
            return false;
        *x = s->p[0] - '0';
        s->p++;
        s->len--;
        return true;
    }
    *x = (s->p[0] - '0') * 10 + (s->p[1] - '0');
    s->p += 2;
    s->len -= 2;
    return true;
}

/* Go's getnum3: one to three digits, three when fixed. */
static bool tf_getnum3(Str *s, bool fixed, Int *x) {
    Int n = 0;
    Int i = 0;
    for (; i < 3 && tf_is_digit(*s, i); i++)
        n = n * 10 + (s->p[i] - '0');
    if (i == 0 || (fixed && i != 3)) {
        *x = 0;
        return false;
    }
    *x = n;
    s->p += i;
    s->len -= i;
    return true;
}

/* Go's skip: value past prefix, where a run of spaces in prefix matches any
 * run of spaces in value. */
static bool tf_skip(Str *value, Str prefix) {
    while (prefix.len > 0) {
        if (prefix.p[0] == ' ') {
            if (value->len > 0 && value->p[0] != ' ')
                return false;
            while (prefix.len > 0 && prefix.p[0] == ' ') {
                prefix.p++;
                prefix.len--;
            }
            while (value->len > 0 && value->p[0] == ' ') {
                value->p++;
                value->len--;
            }
            continue;
        }
        if (value->len == 0 || value->p[0] != prefix.p[0])
            return false;
        prefix.p++;
        prefix.len--;
        value->p++;
        value->len--;
    }
    return true;
}

static bool tf_comma_or_period(Byte b) {
    return b == '.' || b == ',';
}

/* Go's parseNanoseconds: value starts with the separator and nbytes counts it.
 * range_err is set for a negative fraction, which atoi's sign lets in. */
static bool tf_parse_nanoseconds(Str value, Int nbytes, Int *ns,
                                 const char **range_err) {
    *ns = 0;
    if (!tf_comma_or_period(value.p[0]))
        return false;
    if (nbytes > 10)
        nbytes = 10;
    if (!tf_atoi(str_from_bytes(value.p + 1, nbytes - 1), ns))
        return false;
    if (*ns < 0) {
        *range_err = "fractional second";
        return true;
    }
    for (Int i = 0; i < 10 - nbytes; i++)
        *ns *= 10;
    return true;
}

/* Go's parseSignedOffset: the length of a sign and an hour from -23 to +23,
 * or 0. */
static Int tf_parse_signed_offset(Str value) {
    Byte sign = value.p[0];
    if (sign != '-' && sign != '+')
        return 0;
    Str digits = str_from_bytes(value.p + 1, value.len - 1);
    uint64_t x;
    Str rem;
    if (!tf_leading_int(digits, &x, &rem) || rem.len == digits.len)
        return 0;
    if (x > 23)
        return 0;
    return value.len - rem.len;
}

bool burrow__time_parse_time_zone(Str value, Int *length) {
    *length = 0;
    if (value.len < 3)
        return false;
    /* ChST and MeST are the only zones with a lower case letter. */
    if (tf_has(value, 0, "ChST") || tf_has(value, 0, "MeST")) {
        *length = 4;
        return true;
    }
    /* GMT may have an hour offset after it. */
    if (tf_has(value, 0, "GMT")) {
        *length = 3;
        if (value.len > 3)
            *length +=
                tf_parse_signed_offset(str_from_bytes(value.p + 3, value.len - 3));
        return true;
    }
    /* Some zones have no name and are written +03 or -04. */
    if (value.p[0] == '+' || value.p[0] == '-') {
        *length = tf_parse_signed_offset(value);
        return *length > 0;
    }
    /* Three upper case letters, or four or five ending in T. */
    Int upper = 0;
    for (; upper < 6; upper++) {
        if (upper >= value.len)
            break;
        Byte c = value.p[upper];
        if (c < 'A' || 'Z' < c)
            break;
    }
    switch (upper) {
    case 5:
        if (value.p[4] == 'T') {
            *length = 5;
            return true;
        }
        break;
    case 4:
        if (value.p[3] == 'T' || tf_has(value, 0, "WITA")) {
            *length = 4;
            return true;
        }
        break;
    case 3:
        *length = 3;
        return true;
    default:
        break;
    }
    return false;
}

/* Go's parseRFC3339. ok goes false on anything out of place, and the caller
 * falls back to the general parser for the error. */
typedef struct TfRfc {
    bool ok;
} TfRfc;

static Int tf_parse_uint(TfRfc *r, Str s, Int lo, Int hi) {
    Int x = 0;
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        if (c < '0' || '9' < c) {
            r->ok = false;
            return lo;
        }
        x = x * 10 + (c - '0');
    }
    if (x < lo || hi < x) {
        r->ok = false;
        return lo;
    }
    return x;
}

static Str tf_sub(Str s, Int i, Int j) {
    return str_from_bytes(s.p + i, j - i);
}

static bool tf_parse_rfc3339(Str s, TimeLocation *local, Alloc *a, Time *out,
                             Error *err) {
    TfRfc r = {true};
    if (s.len < 19)
        return false;
    Int year = tf_parse_uint(&r, tf_sub(s, 0, 4), 0, 9999);
    Int month = tf_parse_uint(&r, tf_sub(s, 5, 7), 1, 12);
    Int day = tf_parse_uint(&r, tf_sub(s, 8, 10), 1, burrow__time_days_in(month, year));
    Int hour = tf_parse_uint(&r, tf_sub(s, 11, 13), 0, 23);
    Int min = tf_parse_uint(&r, tf_sub(s, 14, 16), 0, 59);
    Int sec = tf_parse_uint(&r, tf_sub(s, 17, 19), 0, 59);
    if (!r.ok || !(s.p[4] == '-' && s.p[7] == '-' && s.p[10] == 'T' && s.p[13] == ':' &&
                   s.p[16] == ':'))
        return false;
    s = tf_sub(s, 19, s.len);

    Int nsec = 0;
    if (s.len >= 2 && s.p[0] == '.' && tf_is_digit(s, 1)) {
        Int n = 2;
        while (n < s.len && tf_is_digit(s, n))
            n++;
        const char *ignored = NULL;
        (void)tf_parse_nanoseconds(s, n, &nsec, &ignored);
        s = tf_sub(s, n, s.len);
    }

    Time t = time_date(year, month, day, hour, min, sec, nsec, time_utc_loc);
    if (s.len != 1 || s.p[0] != 'Z') {
        if (s.len != 6)
            return false;
        Int hr = tf_parse_uint(&r, tf_sub(s, 1, 3), 0, 23);
        Int mm = tf_parse_uint(&r, tf_sub(s, 4, 6), 0, 59);
        if (!r.ok || !((s.p[0] == '-' || s.p[0] == '+') && s.p[3] == ':'))
            return false;
        Int zone_offset = (hr * 60 + mm) * 60;
        if (s.p[0] == '-')
            zone_offset = -zone_offset;
        burrow__time_add_sec(&t, -(int64_t)zone_offset);

        /* Local, when the offset is the one Local has then. */
        if (burrow__time_lookup(local, burrow__time_unix_sec(&t)).offset ==
            zone_offset) {
            burrow__time_set_loc(&t, local);
        } else {
            TimeLocation *l = burrow__time_fixed_zone(a, BURROW_STR_EMPTY, zone_offset);
            if (l == NULL) {
                *err = burrow_err_out_of_memory;
                *out = (Time){0};
                return true;
            }
            burrow__time_set_loc(&t, l);
        }
    }
    *out = t;
    return true;
}

/* Go's parse. default_loc is where a time with no zone in it goes, and local
 * is the location a zone offset or name is matched against. */
static Time tf_parse(Alloc *a, Str layout, Str value, TimeLocation *default_loc,
                     TimeLocation *local, Error *err) {
    Str alayout = layout;
    Str avalue = value;
    const char *range_err = NULL;
    bool am_set = false;
    bool pm_set = false;

    Int year = 0;
    Int month = -1;
    Int day = -1;
    Int yday = -1;
    Int hour = 0;
    Int min = 0;
    Int sec = 0;
    Int nsec = 0;
    TimeLocation *z = NULL;
    Int zone_offset = -1;
    Str zone_name = BURROW_STR_EMPTY;

    for (;;) {
        Str prefix;
        Str suffix;
        int std = tf_next_chunk(layout, &prefix, &suffix);
        Str stdstr = tf_sub(layout, prefix.len, layout.len - suffix.len);
        if (!tf_skip(&value, prefix)) {
            *err = tf_parse_error(alayout, avalue, prefix, value, NULL, NULL);
            return (Time){0};
        }
        if (std == 0) {
            if (value.len != 0) {
                *err = tf_parse_error(alayout, avalue, BURROW_STR_EMPTY, value,
                                      ": extra text: ", &value);
                return (Time){0};
            }
            break;
        }
        layout = suffix;
        Str hold = value;
        bool ok = true;
        Int ignored;
        switch (std & TF_MASK) {
        case TF_YEAR:
            if (value.len < 2) {
                ok = false;
                break;
            }
            ok = tf_atoi(tf_sub(value, 0, 2), &year);
            value = tf_sub(value, 2, value.len);
            if (!ok)
                break;
            /* Unix time starts on December 31 1969 in some time zones. */
            year += year >= 69 ? 1900 : 2000;
            break;
        case TF_LONG_YEAR:
            if (value.len < 4 || !tf_is_digit(value, 0)) {
                ok = false;
                break;
            }
            ok = tf_atoi(tf_sub(value, 0, 4), &year);
            value = tf_sub(value, 4, value.len);
            break;
        case TF_MONTH:
            ok = tf_lookup(tf_short_month_names, 12, &value, &month);
            month++;
            break;
        case TF_LONG_MONTH:
            ok = tf_lookup(tf_long_month_names, 12, &value, &month);
            month++;
            break;
        case TF_NUM_MONTH:
        case TF_ZERO_MONTH:
            ok = tf_getnum(&value, std == TF_ZERO_MONTH, &month);
            if (ok && (month <= 0 || 12 < month))
                range_err = "month";
            break;
        case TF_WEEK_DAY:
            /* The weekday is checked and then ignored. */
            ok = tf_lookup(tf_short_day_names, 7, &value, &ignored);
            break;
        case TF_LONG_WEEK_DAY:
            ok = tf_lookup(tf_long_day_names, 7, &value, &ignored);
            break;
        case TF_DAY:
        case TF_UNDER_DAY:
        case TF_ZERO_DAY:
            if (std == TF_UNDER_DAY && value.len > 0 && value.p[0] == ' ')
                value = tf_sub(value, 1, value.len);
            /* Any one or two digit day, checked against the month later. */
            ok = tf_getnum(&value, std == TF_ZERO_DAY, &day);
            break;
        case TF_UNDER_YEAR_DAY:
        case TF_ZERO_YEAR_DAY:
            for (int i = 0; i < 2; i++) {
                if (std == TF_UNDER_YEAR_DAY && value.len > 0 && value.p[0] == ' ')
                    value = tf_sub(value, 1, value.len);
            }
            ok = tf_getnum3(&value, std == TF_ZERO_YEAR_DAY, &yday);
            break;
        case TF_HOUR:
            ok = tf_getnum(&value, false, &hour);
            if (hour < 0 || 24 <= hour)
                range_err = "hour";
            break;
        case TF_HOUR12:
        case TF_ZERO_HOUR12:
            ok = tf_getnum(&value, std == TF_ZERO_HOUR12, &hour);
            if (hour < 0 || 12 < hour)
                range_err = "hour";
            break;
        case TF_MINUTE:
        case TF_ZERO_MINUTE:
            ok = tf_getnum(&value, std == TF_ZERO_MINUTE, &min);
            if (min < 0 || 60 <= min)
                range_err = "minute";
            break;
        case TF_SECOND:
        case TF_ZERO_SECOND: {
            ok = tf_getnum(&value, std == TF_ZERO_SECOND, &sec);
            if (!ok)
                break;
            if (sec < 0 || 60 <= sec) {
                range_err = "second";
                break;
            }
            /* A fraction in the value with none in the layout is read
             * anyway. */
            if (value.len >= 2 && tf_comma_or_period(value.p[0]) &&
                tf_is_digit(value, 1)) {
                Str p2;
                Str s2;
                int next = tf_next_chunk(layout, &p2, &s2) & TF_MASK;
                if (next == TF_FRAC_SECOND0 || next == TF_FRAC_SECOND9)
                    break;
                Int n = 2;
                while (n < value.len && tf_is_digit(value, n))
                    n++;
                ok = tf_parse_nanoseconds(value, n, &nsec, &range_err);
                value = tf_sub(value, n, value.len);
            }
            break;
        }
        case TF_PM:
        case TF_PM_LOWER: {
            if (value.len < 2) {
                ok = false;
                break;
            }
            Str p = tf_sub(value, 0, 2);
            value = tf_sub(value, 2, value.len);
            bool upper = (std & TF_MASK) == TF_PM;
            if (str_eq(p, upper ? BURROW_S("PM") : BURROW_S("pm")))
                pm_set = true;
            else if (str_eq(p, upper ? BURROW_S("AM") : BURROW_S("am")))
                am_set = true;
            else
                ok = false;
            break;
        }
        case TF_ISO8601_TZ:
        case TF_ISO8601_SHORT_TZ:
        case TF_ISO8601_COLON_TZ:
        case TF_ISO8601_SECONDS_TZ:
        case TF_ISO8601_COLON_SECONDS_TZ:
        case TF_NUM_TZ:
        case TF_NUM_SHORT_TZ:
        case TF_NUM_COLON_TZ:
        case TF_NUM_SECONDS_TZ:
        case TF_NUM_COLON_SECONDS_TZ: {
            if (tf_is_iso_tz(std) && value.len >= 1 && value.p[0] == 'Z') {
                value = tf_sub(value, 1, value.len);
                z = time_utc_loc;
                break;
            }
            Str sign;
            Str hh;
            Str mm;
            Str ss = BURROW_S("00");
            if (std == TF_ISO8601_COLON_TZ || std == TF_NUM_COLON_TZ) {
                if (value.len < 6 || value.p[3] != ':') {
                    ok = false;
                    break;
                }
                sign = tf_sub(value, 0, 1);
                hh = tf_sub(value, 1, 3);
                mm = tf_sub(value, 4, 6);
                value = tf_sub(value, 6, value.len);
            } else if (std == TF_NUM_SHORT_TZ || std == TF_ISO8601_SHORT_TZ) {
                if (value.len < 3) {
                    ok = false;
                    break;
                }
                sign = tf_sub(value, 0, 1);
                hh = tf_sub(value, 1, 3);
                mm = BURROW_S("00");
                value = tf_sub(value, 3, value.len);
            } else if (std == TF_ISO8601_COLON_SECONDS_TZ ||
                       std == TF_NUM_COLON_SECONDS_TZ) {
                if (value.len < 9 || value.p[3] != ':' || value.p[6] != ':') {
                    ok = false;
                    break;
                }
                sign = tf_sub(value, 0, 1);
                hh = tf_sub(value, 1, 3);
                mm = tf_sub(value, 4, 6);
                ss = tf_sub(value, 7, 9);
                value = tf_sub(value, 9, value.len);
            } else if (std == TF_ISO8601_SECONDS_TZ || std == TF_NUM_SECONDS_TZ) {
                if (value.len < 7) {
                    ok = false;
                    break;
                }
                sign = tf_sub(value, 0, 1);
                hh = tf_sub(value, 1, 3);
                mm = tf_sub(value, 3, 5);
                ss = tf_sub(value, 5, 7);
                value = tf_sub(value, 7, value.len);
            } else {
                if (value.len < 5) {
                    ok = false;
                    break;
                }
                sign = tf_sub(value, 0, 1);
                hh = tf_sub(value, 1, 3);
                mm = tf_sub(value, 3, 5);
                value = tf_sub(value, 5, value.len);
            }
            Int hr = 0;
            Int mi = 0;
            Int se = 0;
            ok = tf_getnum(&hh, true, &hr);
            if (ok) {
                ok = tf_getnum(&mm, true, &mi);
                if (ok)
                    ok = tf_getnum(&ss, true, &se);
            }
            /* Greater than rather than at least, because people do write
             * offsets of 24 hours, or 60 minutes or seconds. */
            if (hr > 24)
                range_err = "time zone offset hour";
            if (mi > 60)
                range_err = "time zone offset minute";
            if (se > 60)
                range_err = "time zone offset second";
            zone_offset = (hr * 60 + mi) * 60 + se;
            if (sign.p[0] == '-')
                zone_offset = -zone_offset;
            else if (sign.p[0] != '+')
                ok = false;
            break;
        }
        case TF_TZ: {
            if (tf_has(value, 0, "UTC")) {
                z = time_utc_loc;
                value = tf_sub(value, 3, value.len);
                break;
            }
            Int n;
            if (!burrow__time_parse_time_zone(value, &n)) {
                ok = false;
                break;
            }
            zone_name = tf_sub(value, 0, n);
            value = tf_sub(value, n, value.len);
            break;
        }
        case TF_FRAC_SECOND0: {
            /* The exact number of digits the layout has. */
            Int ndigit = 1 + tf_digits_len(std);
            if (value.len < ndigit) {
                ok = false;
                break;
            }
            ok = tf_parse_nanoseconds(value, ndigit, &nsec, &range_err);
            value = tf_sub(value, ndigit, value.len);
            break;
        }
        case TF_FRAC_SECOND9: {
            if (value.len < 2 || !tf_comma_or_period(value.p[0]) || value.p[1] < '0' ||
                '9' < value.p[1])
                break; /* no fraction, which is fine */
            /* Any number of digits, even more than the layout has, which is
             * what a second with no fraction in the layout does too. */
            Int i = 0;
            while (i + 1 < value.len && '0' <= value.p[i + 1] && value.p[i + 1] <= '9')
                i++;
            ok = tf_parse_nanoseconds(value, 1 + i, &nsec, &range_err);
            value = tf_sub(value, 1 + i, value.len);
            break;
        }
        default:
            break;
        }
        if (range_err != NULL) {
            Byte msg[48];
            size_t rl = strlen(range_err);
            msg[0] = ':';
            msg[1] = ' ';
            memcpy(msg + 2, range_err, rl);
            memcpy(msg + 2 + rl, " out of range", 14);
            *err =
                tf_parse_error(alayout, avalue, stdstr, value, (const char *)msg, NULL);
            return (Time){0};
        }
        if (!ok) {
            *err = tf_parse_error(alayout, avalue, stdstr, hold, NULL, NULL);
            return (Time){0};
        }
    }
    if (pm_set && hour < 12)
        hour += 12;
    else if (am_set && hour == 12)
        hour = 0;

    /* A day of the year becomes a month and a day. */
    if (yday >= 0) {
        Int d = 0;
        Int m = 0;
        if (burrow__time_is_leap(year)) {
            if (yday == 31 + 29) {
                m = TIME_FEBRUARY;
                d = 29;
            } else if (yday > 31 + 29) {
                yday--;
            }
        }
        if (yday < 1 || yday > 365) {
            *err = tf_parse_error(alayout, avalue, BURROW_STR_EMPTY, value,
                                  ": day-of-year out of range", NULL);
            return (Time){0};
        }
        if (m == 0) {
            m = (yday - 1) / 31 + 1;
            if (burrow__time_days_before(m + 1) < yday)
                m++;
            d = yday - burrow__time_days_before(m);
        }
        /* A month and day already seen have to agree with it. */
        if (month >= 0 && month != m) {
            *err = tf_parse_error(alayout, avalue, BURROW_STR_EMPTY, value,
                                  ": day-of-year does not match month", NULL);
            return (Time){0};
        }
        month = m;
        if (day >= 0 && day != d) {
            *err = tf_parse_error(alayout, avalue, BURROW_STR_EMPTY, value,
                                  ": day-of-year does not match day", NULL);
            return (Time){0};
        }
        day = d;
    } else {
        if (month < 0)
            month = TIME_JANUARY;
        if (day < 0)
            day = 1;
    }

    if (day < 1 || day > burrow__time_days_in(month, year)) {
        *err = tf_parse_error(alayout, avalue, BURROW_STR_EMPTY, value,
                              ": day out of range", NULL);
        return (Time){0};
    }

    if (z != NULL)
        return time_date(year, month, day, hour, min, sec, nsec, z);

    if (zone_offset != -1) {
        Time t = time_date(year, month, day, hour, min, sec, nsec, time_utc_loc);
        burrow__time_add_sec(&t, -(int64_t)zone_offset);

        /* Local's zone, when it has this offset at this time. */
        TzLookup lz = burrow__time_lookup(local, burrow__time_unix_sec(&t));
        if (lz.offset == zone_offset &&
            (zone_name.len == 0 || str_eq(lz.name, zone_name))) {
            burrow__time_set_loc(&t, local);
            return t;
        }

        /* Otherwise a made up zone that records the offset. */
        TimeLocation *l = burrow__time_fixed_zone(a, zone_name, zone_offset);
        if (l == NULL) {
            *err = burrow_err_out_of_memory;
            return (Time){0};
        }
        burrow__time_set_loc(&t, l);
        return t;
    }

    if (zone_name.len > 0) {
        Time t = time_date(year, month, day, hour, min, sec, nsec, time_utc_loc);
        /* Local's zone of that name, when it had it at this time. */
        Int offset = 0;
        if (burrow__time_lookup_name(local, zone_name, burrow__time_unix_sec(&t),
                                     &offset)) {
            burrow__time_add_sec(&t, -(int64_t)offset);
            burrow__time_set_loc(&t, local);
            return t;
        }

        /* Otherwise a made up zone with the name and, unless it is GMT with
         * an hour after it, an offset of zero. */
        if (zone_name.len > 3 && tf_has(zone_name, 0, "GMT")) {
            (void)tf_atoi(tf_sub(zone_name, 3, zone_name.len), &offset);
            offset *= 3600;
        }
        TimeLocation *l = burrow__time_fixed_zone(a, zone_name, offset);
        if (l == NULL) {
            *err = burrow_err_out_of_memory;
            return (Time){0};
        }
        burrow__time_set_loc(&t, l);
        return t;
    }

    return time_date(year, month, day, hour, min, sec, nsec, default_loc);
}

static bool tf_is_rfc3339_layout(Str layout) {
    return str_eq(layout, TIME_RFC3339) || str_eq(layout, TIME_RFC3339_NANO);
}

Time time_parse(Alloc *a, Str layout, Str value, Error *err) {
    Error e = BURROW_NO_ERROR;
    Time t;
    if (!tf_is_rfc3339_layout(layout) ||
        !tf_parse_rfc3339(value, time_local_loc, a, &t, &e))
        t = tf_parse(a, layout, value, time_utc_loc, time_local_loc, &e);
    BURROW_OUT(err, e);
    return t;
}

Time time_parse_in_location(Alloc *a, Str layout, Str value, TimeLocation *loc,
                            Error *err) {
    Error e = BURROW_NO_ERROR;
    Time t;
    if (!tf_is_rfc3339_layout(layout) || !tf_parse_rfc3339(value, loc, a, &t, &e))
        t = tf_parse(a, layout, value, loc, loc, &e);
    BURROW_OUT(err, e);
    return t;
}

/* ------------------------------------------------------- text and JSON */

/* Go's appendStrictRFC3339: RFC3339Nano, and an error for the two things RFC
 * 3339 can not say, a year that is not four digits and a zone offset of a day
 * or more. */
static const char *tf_strict_rfc3339(TfOut *o, Time t) {
    Int n0 = o->n;
    tf_format_rfc3339(o, t, true);
    /* Every byte looked at here is in the buffer: the callers give it room for
     * the longest Time there is. */
    const Byte *b = o->p;
    Int n = o->n;
    if (b[n0 + 4] != '-')
        return "year outside of range [0,9999]";
    if (b[n - 1] != 'Z') {
        Byte c = b[n - 6];
        Int hh = (b[n - 5] - '0') * 10 + (b[n - 4] - '0');
        if (('0' <= c && c <= '9') || hh >= 24)
            return "timezone hour outside of range [0,23]";
    }
    return NULL;
}

/* The longest RFC3339Nano text a Time can have: an eleven digit year with a
 * sign, and an offset of more than a hundred hours. */
enum { TF_RFC3339_MAX = 64 };

static Error tf_prefixed(const char *prefix, const char *msg) {
    size_t pl = strlen(prefix);
    size_t ml = strlen(msg);
    Byte buf[96];
    memcpy(buf, prefix, pl);
    memcpy(buf + pl, msg, ml);
    return errors_new(error_allocator(), str_from_bytes(buf, (Int)pl + (Int)ml));
}

static Slice tf_append_to(Time t, Alloc *a, Slice b, Error *err, const char *prefix,
                          bool quoted) {
    Byte buf[TF_RFC3339_MAX + 2];
    TfOut o = {buf, 0, (Int)sizeof buf};
    if (quoted)
        tf_putc(&o, '"');
    const char *msg = tf_strict_rfc3339(&o, t);
    if (msg != NULL) {
        BURROW_OUT(err, tf_prefixed(prefix, msg));
        return b;
    }
    if (quoted)
        tf_putc(&o, '"');
    if (b.elem == NULL)
        b = slice_nil(TYPE_BYTE);
    Slice out = slice_append(a, b, buf, o.n);
    if (out.len != b.len + o.n) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return b;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return out;
}

Slice time_append_text(Time t, Alloc *a, Slice b, Error *err) {
    return tf_append_to(t, a, b, err, "Time.AppendText: ", false);
}

Slice time_marshal_text(Time t, Alloc *a, Error *err) {
    return tf_append_to(t, a, slice_nil(TYPE_BYTE), err, "Time.MarshalText: ", false);
}

Slice time_marshal_json(Time t, Alloc *a, Error *err) {
    return tf_append_to(t, a, slice_nil(TYPE_BYTE), err, "Time.MarshalJSON: ", true);
}

/* Go's parseStrictRFC3339, whose strict checks Go has switched off for now,
 * so it is parseRFC3339 with Parse behind it for the error. */
static Error tf_parse_strict_rfc3339(Time *t, Alloc *a, Str s) {
    Error e = BURROW_NO_ERROR;
    Time r;
    if (!tf_parse_rfc3339(s, time_local_loc, a, &r, &e))
        r = tf_parse(a, TIME_RFC3339, s, time_utc_loc, time_local_loc, &e);
    *t = BURROW_FAILED(e) ? (Time){0} : r;
    return e;
}

Error time_unmarshal_text(Time *t, Alloc *a, Slice data) {
    return tf_parse_strict_rfc3339(t, a, str_from_bytes(data.p, data.len));
}

Error time_unmarshal_json(Time *t, Alloc *a, Slice data) {
    Str s = str_from_bytes(data.p, data.len);
    if (str_eq(s, BURROW_S("null")))
        return BURROW_NO_ERROR;
    if (s.len < 2 || s.p[0] != '"' || s.p[s.len - 1] != '"')
        return errors_new(error_allocator(),
                          BURROW_S("Time.UnmarshalJSON: input is not a JSON string"));
    return tf_parse_strict_rfc3339(t, a, tf_sub(s, 1, s.len - 1));
}

/* ------------------------------------------------------------ descriptor */

/* The methods a Time answers to, which is what lets fmt print one, and the
 * encoders find its text, JSON, binary and gob forms. They take the receiver
 * by pointer, as a descriptor wants, and String and GoString put their text in
 * the goroutine's error arena, having no allocator to take. */
static Slice tf_m_append_binary(Time *self, Alloc *a, Slice b, Error *err) {
    return time_append_binary(*self, a, b, err);
}
static Slice tf_m_append_text(Time *self, Alloc *a, Slice b, Error *err) {
    return time_append_text(*self, a, b, err);
}
static Error tf_m_gob_decode(Time *self, Alloc *a, Slice data) {
    return time_gob_decode(self, a, data);
}
static Slice tf_m_gob_encode(Time *self, Alloc *a, Error *err) {
    return time_gob_encode(*self, a, err);
}
static Str tf_m_go_string(Time *self) {
    return time_go_string(*self, error_allocator());
}
static Slice tf_m_marshal_binary(Time *self, Alloc *a, Error *err) {
    return time_marshal_binary(*self, a, err);
}
static Slice tf_m_marshal_json(Time *self, Alloc *a, Error *err) {
    return time_marshal_json(*self, a, err);
}
static Slice tf_m_marshal_text(Time *self, Alloc *a, Error *err) {
    return time_marshal_text(*self, a, err);
}
static Str tf_m_string(Time *self) {
    return time_string(*self, error_allocator());
}
static Error tf_m_unmarshal_binary(Time *self, Alloc *a, Slice data) {
    return time_unmarshal_binary(self, a, data);
}
static Error tf_m_unmarshal_json(Time *self, Alloc *a, Slice data) {
    return time_unmarshal_json(self, a, data);
}
static Error tf_m_unmarshal_text(Time *self, Alloc *a, Slice data) {
    return time_unmarshal_text(self, a, data);
}

#define TF_SIG_STRING(IN, OUT) OUT(Str)
#define TF_SIG_GOB_ENCODE(IN, OUT) ENCODING_SIG_MARSHAL_BINARY(IN, OUT)
#define TF_SIG_GOB_DECODE(IN, OUT) ENCODING_SIG_UNMARSHAL_BINARY(IN, OUT)
#define TF_METHODS(M, T)                                                               \
    M(T, AppendBinary, tf_m_append_binary, ENCODING_SIG_APPEND_BINARY)                 \
    M(T, AppendText, tf_m_append_text, ENCODING_SIG_APPEND_TEXT)                       \
    M(T, GoString, tf_m_go_string, TF_SIG_STRING)                                      \
    M(T, GobDecode, tf_m_gob_decode, TF_SIG_GOB_DECODE)                                \
    M(T, GobEncode, tf_m_gob_encode, TF_SIG_GOB_ENCODE)                                \
    M(T, MarshalBinary, tf_m_marshal_binary, ENCODING_SIG_MARSHAL_BINARY)              \
    M(T, MarshalJSON, tf_m_marshal_json, ENCODING_SIG_MARSHAL_BINARY)                  \
    M(T, MarshalText, tf_m_marshal_text, ENCODING_SIG_MARSHAL_TEXT)                    \
    M(T, String, tf_m_string, TF_SIG_STRING)                                           \
    M(T, UnmarshalBinary, tf_m_unmarshal_binary, ENCODING_SIG_UNMARSHAL_BINARY)        \
    M(T, UnmarshalJSON, tf_m_unmarshal_json, ENCODING_SIG_UNMARSHAL_BINARY)            \
    M(T, UnmarshalText, tf_m_unmarshal_text, ENCODING_SIG_UNMARSHAL_TEXT)

BURROW_METHODS_DEFINE(Time, TF_METHODS);

/* Go's fields are wall, ext and loc, all unexported, so no encoder looks at
 * them. loc is a *Location in Go and an unsafe pointer here, since Location
 * has no descriptor of its own. */
static const Field tf_fields[] = {
    {{(const Byte *)"wall", 4},
     {NULL, 0},
     &burrow_type_uint64_t,
     (uint32_t)offsetof(Time, wall)},
    {{(const Byte *)"ext", 3},
     {NULL, 0},
     &burrow_type_int64_t,
     (uint32_t)offsetof(Time, ext)},
    {{(const Byte *)"loc", 3},
     {NULL, 0},
     &burrow_type_UnsafePointer,
     (uint32_t)offsetof(Time, loc)},
};

const Type burrow_type_Time = {
    {(const Byte *)"Time", 4},
    {(const Byte *)"time", 4},
    KIND_STRUCT,
    (uint32_t)sizeof(Time),
    (uint16_t)_Alignof(Time),
    (uint16_t)(sizeof tf_fields / sizeof tf_fields[0]),
    (uint16_t)(sizeof(burrow__methods_Time) / sizeof(burrow__methods_Time[0])),
    tf_fields,
    burrow__methods_Time,
    NULL,
    NULL,
    0,
    0x7474696dU, /* "ttim" */
    NULL,
};
