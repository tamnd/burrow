/* Local on Windows: Go's zoneinfo_windows.go and zoneinfo_abbrs_windows.go.
 *
 * Windows describes the local zone as a TIME_ZONE_INFORMATION, two offsets
 * and the rule for when each starts, and Go turns that into a Location with
 * this year's rule applied to the hundred years either side. The building is
 * plain arithmetic, so it is compiled everywhere and its tests run everywhere.
 * Only reading the information, and the registry walk that finds the English
 * name of a zone, need Windows, and those are in src/pal/registry_windows.c.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/time.h"

#include "burrow/core.h"
#include "burrow/mem.h"
#include "burrow/pal.h"

#include "time_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct TzAbbr {
    const char *name;
    const char *std;
    const char *dst;
} TzAbbr;

/* Go's abbrs from zoneinfo_abbrs_windows.go, which genzabbrs.go writes from
 * CLDR's windowsZones.xml: the abbreviations for each Windows zone name,
 * sorted by name so it can be searched. Go source: go1.27.1. */
static const TzAbbr tz_abbrs[] = {
    {"AUS Central Standard Time", "ACST", "ACST"},       /* Australia/Darwin */
    {"AUS Eastern Standard Time", "AEST", "AEDT"},       /* Australia/Sydney */
    {"Afghanistan Standard Time", "+0430", "+0430"},     /* Asia/Kabul */
    {"Alaskan Standard Time", "AKST", "AKDT"},           /* America/Anchorage */
    {"Aleutian Standard Time", "HST", "HDT"},            /* America/Adak */
    {"Altai Standard Time", "+07", "+07"},               /* Asia/Barnaul */
    {"Arab Standard Time", "+03", "+03"},                /* Asia/Riyadh */
    {"Arabian Standard Time", "+04", "+04"},             /* Asia/Dubai */
    {"Arabic Standard Time", "+03", "+03"},              /* Asia/Baghdad */
    {"Argentina Standard Time", "-03", "-03"},           /* America/Buenos_Aires */
    {"Astrakhan Standard Time", "+04", "+04"},           /* Europe/Astrakhan */
    {"Atlantic Standard Time", "AST", "ADT"},            /* America/Halifax */
    {"Aus Central W. Standard Time", "+0845", "+0845"},  /* Australia/Eucla */
    {"Azerbaijan Standard Time", "+04", "+04"},          /* Asia/Baku */
    {"Azores Standard Time", "-01", "+00"},              /* Atlantic/Azores */
    {"Bahia Standard Time", "-03", "-03"},               /* America/Bahia */
    {"Bangladesh Standard Time", "+06", "+06"},          /* Asia/Dhaka */
    {"Belarus Standard Time", "+03", "+03"},             /* Europe/Minsk */
    {"Bougainville Standard Time", "+11", "+11"},        /* Pacific/Bougainville */
    {"Canada Central Standard Time", "CST", "CST"},      /* America/Regina */
    {"Cape Verde Standard Time", "-01", "-01"},          /* Atlantic/Cape_Verde */
    {"Caucasus Standard Time", "+04", "+04"},            /* Asia/Yerevan */
    {"Cen. Australia Standard Time", "ACST", "ACDT"},    /* Australia/Adelaide */
    {"Central America Standard Time", "CST", "CST"},     /* America/Guatemala */
    {"Central Asia Standard Time", "+06", "+06"},        /* Asia/Bishkek */
    {"Central Brazilian Standard Time", "-04", "-04"},   /* America/Cuiaba */
    {"Central Europe Standard Time", "CET", "CEST"},     /* Europe/Budapest */
    {"Central European Standard Time", "CET", "CEST"},   /* Europe/Warsaw */
    {"Central Pacific Standard Time", "+11", "+11"},     /* Pacific/Guadalcanal */
    {"Central Standard Time", "CST", "CDT"},             /* America/Chicago */
    {"Central Standard Time (Mexico)", "CST", "CST"},    /* America/Mexico_City */
    {"Chatham Islands Standard Time", "+1245", "+1345"}, /* Pacific/Chatham */
    {"China Standard Time", "CST", "CST"},               /* Asia/Shanghai */
    {"Cuba Standard Time", "CST", "CDT"},                /* America/Havana */
    {"Dateline Standard Time", "-12", "-12"},            /* Etc/GMT+12 */
    {"E. Africa Standard Time", "EAT", "EAT"},           /* Africa/Nairobi */
    {"E. Australia Standard Time", "AEST", "AEST"},      /* Australia/Brisbane */
    {"E. Europe Standard Time", "EET", "EEST"},          /* Europe/Chisinau */
    {"E. South America Standard Time", "-03", "-03"},    /* America/Sao_Paulo */
    {"Easter Island Standard Time", "-06", "-05"},       /* Pacific/Easter */
    {"Eastern Standard Time", "EST", "EDT"},             /* America/New_York */
    {"Eastern Standard Time (Mexico)", "EST", "EST"},    /* America/Cancun */
    {"Egypt Standard Time", "EET", "EEST"},              /* Africa/Cairo */
    {"Ekaterinburg Standard Time", "+05", "+05"},        /* Asia/Yekaterinburg */
    {"FLE Standard Time", "EET", "EEST"},                /* Europe/Kiev */
    {"Fiji Standard Time", "+12", "+12"},                /* Pacific/Fiji */
    {"GMT Standard Time", "GMT", "BST"},                 /* Europe/London */
    {"GTB Standard Time", "EET", "EEST"},                /* Europe/Bucharest */
    {"Georgian Standard Time", "+04", "+04"},            /* Asia/Tbilisi */
    {"Greenland Standard Time", "-02", "-01"},           /* America/Godthab */
    {"Greenwich Standard Time", "GMT", "GMT"},           /* Atlantic/Reykjavik */
    {"Haiti Standard Time", "EST", "EDT"},               /* America/Port-au-Prince */
    {"Hawaiian Standard Time", "HST", "HST"},            /* Pacific/Honolulu */
    {"India Standard Time", "IST", "IST"},               /* Asia/Calcutta */
    {"Iran Standard Time", "+0330", "+0330"},            /* Asia/Tehran */
    {"Israel Standard Time", "IST", "IDT"},              /* Asia/Jerusalem */
    {"Jordan Standard Time", "+03", "+03"},              /* Asia/Amman */
    {"Kaliningrad Standard Time", "EET", "EET"},         /* Europe/Kaliningrad */
    {"Korea Standard Time", "KST", "KST"},               /* Asia/Seoul */
    {"Libya Standard Time", "EET", "EET"},               /* Africa/Tripoli */
    {"Line Islands Standard Time", "+14", "+14"},        /* Pacific/Kiritimati */
    {"Lord Howe Standard Time", "+1030", "+11"},         /* Australia/Lord_Howe */
    {"Magadan Standard Time", "+11", "+11"},             /* Asia/Magadan */
    {"Magallanes Standard Time", "-03", "-03"},          /* America/Punta_Arenas */
    {"Marquesas Standard Time", "-0930", "-0930"},       /* Pacific/Marquesas */
    {"Mauritius Standard Time", "+04", "+04"},           /* Indian/Mauritius */
    {"Middle East Standard Time", "EET", "EEST"},        /* Asia/Beirut */
    {"Montevideo Standard Time", "-03", "-03"},          /* America/Montevideo */
    {"Morocco Standard Time", "+00", "+01"},             /* Africa/Casablanca */
    {"Mountain Standard Time", "MST", "MDT"},            /* America/Denver */
    {"Mountain Standard Time (Mexico)", "MST", "MST"},   /* America/Mazatlan */
    {"Myanmar Standard Time", "+0630", "+0630"},         /* Asia/Rangoon */
    {"N. Central Asia Standard Time", "+07", "+07"},     /* Asia/Novosibirsk */
    {"Namibia Standard Time", "CAT", "CAT"},             /* Africa/Windhoek */
    {"Nepal Standard Time", "+0545", "+0545"},           /* Asia/Katmandu */
    {"New Zealand Standard Time", "NZST", "NZDT"},       /* Pacific/Auckland */
    {"Newfoundland Standard Time", "NST", "NDT"},        /* America/St_Johns */
    {"Norfolk Standard Time", "+11", "+12"},             /* Pacific/Norfolk */
    {"North Asia East Standard Time", "+08", "+08"},     /* Asia/Irkutsk */
    {"North Asia Standard Time", "+07", "+07"},          /* Asia/Krasnoyarsk */
    {"North Korea Standard Time", "KST", "KST"},         /* Asia/Pyongyang */
    {"Omsk Standard Time", "+06", "+06"},                /* Asia/Omsk */
    {"Pacific SA Standard Time", "-04", "-03"},          /* America/Santiago */
    {"Pacific Standard Time", "PST", "PDT"},             /* America/Los_Angeles */
    {"Pacific Standard Time (Mexico)", "PST", "PDT"},    /* America/Tijuana */
    {"Pakistan Standard Time", "PKT", "PKT"},            /* Asia/Karachi */
    {"Paraguay Standard Time", "-04", "-03"},            /* America/Asuncion */
    {"Qyzylorda Standard Time", "+05", "+05"},           /* Asia/Qyzylorda */
    {"Romance Standard Time", "CET", "CEST"},            /* Europe/Paris */
    {"Russia Time Zone 10", "+11", "+11"},               /* Asia/Srednekolymsk */
    {"Russia Time Zone 11", "+12", "+12"},               /* Asia/Kamchatka */
    {"Russia Time Zone 3", "+04", "+04"},                /* Europe/Samara */
    {"Russian Standard Time", "MSK", "MSK"},             /* Europe/Moscow */
    {"SA Eastern Standard Time", "-03", "-03"},          /* America/Cayenne */
    {"SA Pacific Standard Time", "-05", "-05"},          /* America/Bogota */
    {"SA Western Standard Time", "-04", "-04"},          /* America/La_Paz */
    {"SE Asia Standard Time", "+07", "+07"},             /* Asia/Bangkok */
    {"Saint Pierre Standard Time", "-03", "-02"},        /* America/Miquelon */
    {"Sakhalin Standard Time", "+11", "+11"},            /* Asia/Sakhalin */
    {"Samoa Standard Time", "+13", "+13"},               /* Pacific/Apia */
    {"Sao Tome Standard Time", "GMT", "GMT"},            /* Africa/Sao_Tome */
    {"Saratov Standard Time", "+04", "+04"},             /* Europe/Saratov */
    {"Singapore Standard Time", "+08", "+08"},           /* Asia/Singapore */
    {"South Africa Standard Time", "SAST", "SAST"},      /* Africa/Johannesburg */
    {"South Sudan Standard Time", "CAT", "CAT"},         /* Africa/Juba */
    {"Sri Lanka Standard Time", "+0530", "+0530"},       /* Asia/Colombo */
    {"Sudan Standard Time", "CAT", "CAT"},               /* Africa/Khartoum */
    {"Syria Standard Time", "+03", "+03"},               /* Asia/Damascus */
    {"Taipei Standard Time", "CST", "CST"},              /* Asia/Taipei */
    {"Tasmania Standard Time", "AEST", "AEDT"},          /* Australia/Hobart */
    {"Tocantins Standard Time", "-03", "-03"},           /* America/Araguaina */
    {"Tokyo Standard Time", "JST", "JST"},               /* Asia/Tokyo */
    {"Tomsk Standard Time", "+07", "+07"},               /* Asia/Tomsk */
    {"Tonga Standard Time", "+13", "+13"},               /* Pacific/Tongatapu */
    {"Transbaikal Standard Time", "+09", "+09"},         /* Asia/Chita */
    {"Turkey Standard Time", "+03", "+03"},              /* Europe/Istanbul */
    {"Turks And Caicos Standard Time", "EST", "EDT"},    /* America/Grand_Turk */
    {"US Eastern Standard Time", "EST", "EDT"},          /* America/Indianapolis */
    {"US Mountain Standard Time", "MST", "MST"},         /* America/Phoenix */
    {"UTC", "UTC", "UTC"},                               /* Etc/UTC */
    {"UTC+12", "+12", "+12"},                            /* Etc/GMT-12 */
    {"UTC+13", "+13", "+13"},                            /* Etc/GMT-13 */
    {"UTC-02", "-02", "-02"},                            /* Etc/GMT+2 */
    {"UTC-08", "-08", "-08"},                            /* Etc/GMT+8 */
    {"UTC-09", "-09", "-09"},                            /* Etc/GMT+9 */
    {"UTC-11", "-11", "-11"},                            /* Etc/GMT+11 */
    {"Ulaanbaatar Standard Time", "+08", "+08"},         /* Asia/Ulaanbaatar */
    {"Venezuela Standard Time", "-04", "-04"},           /* America/Caracas */
    {"Vladivostok Standard Time", "+10", "+10"},         /* Asia/Vladivostok */
    {"Volgograd Standard Time", "MSK", "MSK"},           /* Europe/Volgograd */
    {"W. Australia Standard Time", "AWST", "AWST"},      /* Australia/Perth */
    {"W. Central Africa Standard Time", "WAT", "WAT"},   /* Africa/Lagos */
    {"W. Europe Standard Time", "CET", "CEST"},          /* Europe/Berlin */
    {"W. Mongolia Standard Time", "+07", "+07"},         /* Asia/Hovd */
    {"West Asia Standard Time", "+05", "+05"},           /* Asia/Tashkent */
    {"West Bank Standard Time", "EET", "EEST"},          /* Asia/Hebron */
    {"West Pacific Standard Time", "+10", "+10"},        /* Pacific/Port_Moresby */
    {"Yakutsk Standard Time", "+09", "+09"},             /* Asia/Yakutsk */
    {"Yukon Standard Time", "MST", "MST"},               /* America/Whitehorse */
};

static bool tz_abbr_find(Str name, const TzAbbr **out) {
    size_t lo = 0, hi = sizeof tz_abbrs / sizeof tz_abbrs[0];
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        Str m = str_from_cstr(tz_abbrs[mid].name);
        int c = str_cmp(m, name);
        if (c == 0) {
            *out = &tz_abbrs[mid];
            return true;
        }
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return false;
}

/* extractCAPS: the capital letters of desc. Every byte of a multibyte UTF-8
 * sequence is 0x80 or more, so looking at bytes finds the same letters Go's
 * rune loop does. Writes into out, which is at least desc.len long. */
static Int tz_extract_caps(Str desc, Byte *out) {
    Int n = 0;
    for (Int i = 0; i < desc.len; i++)
        if (desc.p[i] >= 'A' && desc.p[i] <= 'Z')
            out[n++] = desc.p[i];
    return n;
}

/* pseudoUnix: the second, counted from 1970 in local time, at which the rule
 * d starts in year. Windows gives the rule as day of week d.day_of_week in week
 * d.day of month d.month, where week 5 means the last one. */
static int64_t tz_pseudo_unix(Int year, const PalTzDate *d) {
    Int day = 1;
    Time t = time_date(year, (TimeMonth)d->month, day, (Int)d->hour, (Int)d->minute,
                       (Int)d->second, 0, time_utc_loc);
    Int i = (Int)d->day_of_week - (Int)time_weekday(t);
    if (i < 0)
        i += 7;
    day += i;
    Int week = (Int)d->day - 1;
    if (week < 4) {
        day += week * 7;
    } else {
        /* "Last" instance of the day. */
        day += 4 * 7;
        if (day > burrow__time_days_in((TimeMonth)d->month, year))
            day -= 7;
    }
    return time_unix(t) + (int64_t)(day - 1) * TZ_SECONDS_PER_DAY;
}

enum { TZ_WIN_YEARS = 100, TZ_WIN_NTX = 4 * TZ_WIN_YEARS };

TimeLocation *burrow__time_location_from_tzi(Alloc *a, const PalTzInfo *i) {
    Int nzone = i->standard_date.month > 0 ? 2 : 1;
    Int ntx = nzone == 1 ? 1 : TZ_WIN_NTX;
    Str stdname = {(const Byte *)i->standard_name, i->standard_name_len};
    Str dstname = {(const Byte *)i->daylight_name, i->daylight_name_len};

    /* abbrev: the CLDR abbreviations for the zone's English name, which is
     * the standard name unless Windows is in another language, and the
     * capital letters of the names when there are none. */
    const TzAbbr *ab = NULL;
    if (!tz_abbr_find(stdname, &ab)) {
        char english[PAL_TZ_NAME_MAX];
        int64_t elen = 0;
        if (pal_tz_english_name(i->standard_name, i->standard_name_len,
                                i->daylight_name, i->daylight_name_len, english, &elen,
                                NULL))
            (void)tz_abbr_find((Str){(const Byte *)english, elen}, &ab);
    }

    size_t size = sizeof(TimeLocation) + (size_t)nzone * sizeof(TzZone) +
                  (size_t)ntx * sizeof(TzTrans) + (size_t)stdname.len +
                  (size_t)dstname.len;
    Byte *block = (Byte *)mem_alloc(a, size, _Alignof(TimeLocation));
    if (block == NULL)
        return NULL;
    TimeLocation *l = (TimeLocation *)(void *)block;
    TzZone *zone = (TzZone *)(void *)(block + sizeof(TimeLocation));
    TzTrans *tx = (TzTrans *)(void *)(zone + nzone);
    Byte *names = (Byte *)(tx + ntx);

    Str std, dst;
    if (ab != NULL) {
        std = str_from_cstr(ab->std);
        dst = str_from_cstr(ab->dst);
    } else {
        std = (Str){names, tz_extract_caps(stdname, names)};
        dst = (Str){names + std.len, tz_extract_caps(dstname, names + std.len)};
    }

    l->name = BURROW_S("Local");
    l->zone = zone;
    l->nzone = nzone;
    l->tx = tx;
    l->ntx = ntx;
    l->a = a;
    l->size = size;

    zone[0].name = std;
    if (nzone == 1) {
        /* No daylight savings. */
        zone[0].offset = -(Int)i->bias * 60;
        l->cache_start = TZ_ALPHA;
        l->cache_end = TZ_OMEGA;
        l->cache_zone = &zone[0];
        tx[0].when = l->cache_start;
        tx[0].index = 0;
        return l;
    }

    /* StandardBias must be ignored if StandardDate is not set, so this
     * computation is delayed until after the nzone == 1 return above. */
    zone[0].offset = -(Int)(i->bias + i->standard_bias) * 60;
    zone[1].name = dst;
    zone[1].offset = -(Int)(i->bias + i->daylight_bias) * 60;
    zone[1].is_dst = true;

    /* Arrange so that d0 is first transition date, d1 second, i0 is index of
     * zone after first transition, i1 second. */
    const PalTzDate *d0 = &i->standard_date, *d1 = &i->daylight_date;
    uint8_t i0 = 0, i1 = 1;
    if (d0->month > d1->month) {
        const PalTzDate *d = d0;
        d0 = d1;
        d1 = d;
        i0 = 1;
        i1 = 0;
    }

    /* 2 tx per year, 100 years on each side of this year. */
    Int year = time_year(time_utc(time_now()));
    Int txi = 0;
    for (Int y = year - TZ_WIN_YEARS; y < year + TZ_WIN_YEARS; y++) {
        tx[txi].when = tz_pseudo_unix(y, d0) - zone[i1].offset;
        tx[txi].index = i0;
        txi++;
        tx[txi].when = tz_pseudo_unix(y, d1) - zone[i0].offset;
        tx[txi].index = i1;
        txi++;
    }
    return l;
}
