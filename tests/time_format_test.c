/* tests/time_format_test_gen.h, from tools/gen-time-format-tests.sh, holds what
 * Go's Format says about a list of instants in a list of zones for a list of
 * layouts, what Parse and ParseInLocation make of the results and of values
 * with a byte cut or changed, Go's own parse tables, and String, GoString and
 * the text and JSON encodings. Go's tests run with Local set to Los Angeles
 * under the name "Local", and so does this one, which is what lets the answers
 * be the same on every machine.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/time/time_internal.h"

#include "burrow/encoding/json.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/slice.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stddef.h>
#include <stdint.h>
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

typedef struct FCase {
    long long loc;
    int64_t sec;
    long long nsec;
    long long layout;
    const char *out;
} FCase;

typedef struct RCase {
    long long fidx;
    long long loc;
    const char *err;
    int64_t unix;
    long long nsec;
    const char *zone;
    long long off;
    const char *locname;
} RCase;

typedef struct PCase {
    const char *layout;
    const char *value;
    long long loc;
    const char *err;
    const char *layout_elem;
    const char *value_elem;
    const char *message;
    int64_t unix;
    long long nsec;
    const char *zone;
    long long off;
    const char *locname;
} PCase;

typedef struct SCase {
    long long loc;
    int64_t sec;
    long long nsec;
    const char *string;
    const char *go_string;
    const char *text;
    const char *text_err;
    const char *json;
    const char *json_err;
} SCase;

typedef struct UCase {
    long long kind;
    const char *input;
    const char *err;
    long long unchanged;
    int64_t unix;
    long long nsec;
    const char *zone;
    long long off;
    const char *locname;
} UCase;

typedef struct ZCase {
    const char *value;
    long long length;
    long long ok;
} ZCase;

#include "time_format_test_gen.h"

#define LEN(x) (sizeof(x) / sizeof((x)[0]))
#define NZONES ((int)LEN(g_zones))
#define NLOCS (NZONES + (int)LEN(g_fixed))

/* Parse rather than ParseInLocation, in a case's loc. */
enum { PARSE_ONLY = -3, LOC_LOCAL = -2, LOC_UTC = -1 };

static Str cstr(const char *s) {
    return (Str){(const Byte *)s, s == NULL ? 0 : (Int)strlen(s)};
}

static Str err_str(Error err) {
    return BURROW_FAILED(err) ? error_text(err) : cstr("<nil>");
}

static Str want_err(const char *s) {
    return cstr(s == NULL ? "<nil>" : s);
}

static Slice blob(const char *p, long long n) {
    return slice_from((void *)(uintptr_t)p, (Int)n, (Int)n, TYPE_BYTE);
}

static TimeLocation *g_locs[64];

/* The zones, the fixed zones, and Local made Los Angeles, once. */
static void load_locs(TestingT *t) {
    if (g_locs[0] != NULL)
        return;
    for (int i = 0; i < NZONES; i++) {
        Error err = BURROW_NO_ERROR;
        g_locs[i] = time_load_location_from_tz_data(
            heap_allocator(), cstr(g_zones[i].name),
            blob(g_zones[i].data, g_zones[i].len), &err);
        if (g_locs[i] == NULL)
            testing_t_fatalf_v(t, "%s: %s", g_zones[i].name, err_str(err));
    }
    for (int i = 0; i < (int)LEN(g_fixed); i++)
        g_locs[NZONES + i] =
            time_fixed_zone(heap_allocator(), cstr(g_fixed[i].name), g_fixed[i].off);
    burrow__time_force_local(g_locs[0]);
}

static TimeLocation *loc_of(long long i) {
    if (i == LOC_LOCAL)
        return time_local_loc;
    if (i == LOC_UTC)
        return time_utc_loc;
    return g_locs[i];
}

static Time at(long long loc, int64_t sec, long long nsec) {
    return time_in(time_from_unix(sec, nsec), loc_of(loc));
}

/* Compares what a parse gave with what Go's gave. */
static void check_parse(TestingT *t, const char *what, Str layout, Str value, Time got,
                        Error err, const char *werr, int64_t unix, long long nsec,
                        const char *zone, long long off, const char *locname) {
    if (!str_eq(err_str(err), want_err(werr))) {
        testing_t_errorf_v(t, "%s(%q, %q): error %q, want %q", what, layout, value,
                           err_str(err), want_err(werr));
        return;
    }
    if (werr != NULL) {
        if (!time_is_zero(got))
            testing_t_errorf_v(t, "%s(%q, %q): failed with a nonzero Time", what,
                               layout, value);
        return;
    }
    Int goff = 0;
    Str zn = time_zone(got, &goff);
    Str ln = time_location_string(time_location(got));
    if (time_unix(got) != unix || time_nanosecond(got) != nsec ||
        !str_eq(zn, cstr(zone)) || goff != off || !str_eq(ln, cstr(locname)))
        testing_t_errorf_v(t, "%s(%q, %q) = %d.%d %q %d in %q, want %d.%d %q %d in %q",
                           what, layout, value, time_unix(got), time_nanosecond(got),
                           zn, goff, ln, unix, nsec, zone, off, locname);
}

static Time do_parse(Alloc *a, Str layout, Str value, long long loc, Error *err) {
    if (loc == PARSE_ONLY)
        return time_parse(a, layout, value, err);
    return time_parse_in_location(a, layout, value, loc_of(loc), err);
}

static void TestFormat(TestingT *t) {
    load_locs(t);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < LEN(g_formats); i++) {
        const FCase *c = &g_formats[i];
        Time tm = at(c->loc, c->sec, c->nsec);
        Str layout = cstr(g_layouts[c->layout]);
        Str got = time_format(tm, a, layout);
        if (!str_eq(got, cstr(c->out)))
            testing_t_errorf_v(t, "%d.%d in %q, Format(%q) = %q, want %q", c->sec,
                               c->nsec, time_location_string(loc_of(c->loc)), layout,
                               got, c->out);

        /* AppendFormat onto something, which has to leave it in front. */
        Slice b = slice_append(a, slice_nil(TYPE_BYTE), "x:", 2);
        b = time_append_format(tm, a, b, layout);
        Str sb = {b.p, b.len};
        if (sb.len != got.len + 2 || memcmp(sb.p, "x:", 2) != 0 ||
            memcmp(sb.p + 2, got.p, (size_t)got.len) != 0)
            testing_t_errorf_v(t, "AppendFormat(%q) = %q, want x:%q", layout, sb, got);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void TestParseRoundTrip(TestingT *t) {
    load_locs(t);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < LEN(g_round_trips); i++) {
        const RCase *c = &g_round_trips[i];
        const FCase *f = &g_formats[c->fidx];
        Str layout = cstr(g_layouts[f->layout]);
        Str value = cstr(f->out);
        ArenaMark m = error_mark();
        Error err = BURROW_NO_ERROR;
        Time got = do_parse(a, layout, value, c->loc, &err);
        check_parse(t, c->loc == PARSE_ONLY ? "Parse" : "ParseInLocation", layout,
                    value, got, err, c->err, c->unix, c->nsec, c->zone, c->off,
                    c->locname);
        error_release(m);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void TestParse(TestingT *t) {
    load_locs(t);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < LEN(g_parses); i++) {
        const PCase *c = &g_parses[i];
        Str layout = cstr(c->layout);
        Str value = cstr(c->value);
        ArenaMark m = error_mark();
        Error err = BURROW_NO_ERROR;
        Time got = do_parse(a, layout, value, c->loc, &err);
        check_parse(t, c->loc == PARSE_ONLY ? "Parse" : "ParseInLocation", layout,
                    value, got, err, c->err, c->unix, c->nsec, c->zone, c->off,
                    c->locname);
        if (c->err != NULL && BURROW_FAILED(err)) {
            const TimeParseError *pe = errors_as(err, TYPE_TIME_PARSE_ERROR);
            if (pe == NULL) {
                testing_t_errorf_v(t, "Parse(%q, %q): error is not a TimeParseError",
                                   layout, value);
            } else if (!str_eq(pe->layout, layout) || !str_eq(pe->value, value) ||
                       !str_eq(pe->layout_elem, cstr(c->layout_elem)) ||
                       !str_eq(pe->value_elem, cstr(c->value_elem)) ||
                       !str_eq(pe->message, cstr(c->message))) {
                testing_t_errorf_v(
                    t, "Parse(%q, %q): ParseError fields %q %q %q, want %q %q %q",
                    layout, value, pe->layout_elem, pe->value_elem, pe->message,
                    c->layout_elem, c->value_elem, c->message);
            } else {
                /* The same text out of the struct, and a copy that owns its
                 * strings. */
                Str s = time_parse_error_error(a, pe);
                Error cp = time_parse_error_as_error(a, pe);
                if (!str_eq(s, cstr(c->err)) || !str_eq(err_str(cp), cstr(c->err)) ||
                    errors_as(cp, TYPE_TIME_PARSE_ERROR) == NULL)
                    testing_t_errorf_v(t, "ParseError.Error() = %q, want %q", s,
                                       c->err);
                Error cl = error_retain(a, err);
                if (!str_eq(err_str(cl), cstr(c->err)) ||
                    errors_as(cl, TYPE_TIME_PARSE_ERROR) == NULL)
                    testing_t_errorf_v(t, "retained ParseError = %q, want %q",
                                       err_str(cl), c->err);
            }
        }
        error_release(m);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void TestStringsAndEncodings(TestingT *t) {
    load_locs(t);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < LEN(g_strings); i++) {
        const SCase *c = &g_strings[i];
        Time tm = at(c->loc, c->sec, c->nsec);
        ArenaMark m = error_mark();
        Str s = time_string(tm, a);
        if (!str_eq(s, cstr(c->string)))
            testing_t_errorf_v(t, "String() = %q, want %q", s, c->string);
        s = time_go_string(tm, a);
        if (!str_eq(s, cstr(c->go_string)))
            testing_t_errorf_v(t, "GoString() = %q, want %q", s, c->go_string);

        Error err = BURROW_NO_ERROR;
        Slice b = time_marshal_text(tm, a, &err);
        Str sb = {b.p, b.len};
        if (!str_eq(err_str(err), want_err(c->text_err)) ||
            (c->text_err == NULL && !str_eq(sb, cstr(c->text))))
            testing_t_errorf_v(t, "%q: MarshalText = %q, %q, want %q, %q", c->string,
                               sb, err_str(err), c->text, want_err(c->text_err));

        /* AppendText keeps what is in front, and gives b back on error. */
        Slice pre = slice_append(a, slice_nil(TYPE_BYTE), "t=", 2);
        b = time_append_text(tm, a, pre, &err);
        sb = (Str){b.p, b.len};
        if (c->text_err != NULL) {
            Str w = cstr(c->text_err);
            Str g = err_str(err);
            /* Go's wording, with AppendText where MarshalText was. */
            if (b.len != 2 || g.len != w.len - 1 ||
                memcmp(g.p, "Time.AppendText: ", 17) != 0 ||
                memcmp(g.p + 17, w.p + 18, (size_t)(w.len - 18)) != 0)
                testing_t_errorf_v(t, "%q: AppendText = %q, %q", c->string, sb, g);
        } else if (BURROW_FAILED(err) || sb.len != 2 + (Int)strlen(c->text) ||
                   memcmp(sb.p, "t=", 2) != 0 ||
                   memcmp(sb.p + 2, c->text, strlen(c->text)) != 0) {
            testing_t_errorf_v(t, "%q: AppendText = %q, %q", c->string, sb,
                               err_str(err));
        }

        b = time_marshal_json(tm, a, &err);
        sb = (Str){b.p, b.len};
        if (!str_eq(err_str(err), want_err(c->json_err)) ||
            (c->json_err == NULL && !str_eq(sb, cstr(c->json))))
            testing_t_errorf_v(t, "%q: MarshalJSON = %q, %q, want %q, %q", c->string,
                               sb, err_str(err), c->json, want_err(c->json_err));

        /* And back, which comes to the same instant and offset when the offset
         * is whole minutes, which is all RFC 3339 can write. */
        Int toff = 0;
        (void)time_zone(tm, &toff);
        if (c->json_err == NULL && toff % 60 == 0) {
            Time u = {0, 0, NULL};
            Error e2 = time_unmarshal_json(&u, a, b);
            Int uoff = 0;
            Int off = 0;
            (void)time_zone(u, &uoff);
            (void)time_zone(tm, &off);
            if (BURROW_FAILED(e2) || !time_equal(u, tm) || uoff != off)
                testing_t_errorf_v(t, "%q: UnmarshalJSON gave %d offset %d, %q",
                                   c->string, time_unix(u), uoff, err_str(e2));
        }
        error_release(m);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void TestUnmarshal(TestingT *t) {
    load_locs(t);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Time sentinel = time_date(1999, TIME_SEPTEMBER, 9, 9, 9, 9, 9, time_utc_loc);
    for (size_t i = 0; i < LEN(g_unmarshals); i++) {
        const UCase *c = &g_unmarshals[i];
        const char *what = c->kind == 0 ? "UnmarshalText" : "UnmarshalJSON";
        ArenaMark m = error_mark();
        Time tm = sentinel;
        Slice in = blob(c->input, (long long)strlen(c->input));
        Error err = c->kind == 0 ? time_unmarshal_text(&tm, a, in)
                                 : time_unmarshal_json(&tm, a, in);
        Int off = 0;
        Str zn = time_zone(tm, &off);
        Str ln = time_location_string(time_location(tm));
        if (!str_eq(err_str(err), want_err(c->err)) ||
            time_equal(tm, sentinel) != (c->unchanged != 0) ||
            time_unix(tm) != c->unix || time_nanosecond(tm) != c->nsec ||
            !str_eq(zn, cstr(c->zone)) || off != c->off ||
            !str_eq(ln, cstr(c->locname)))
            testing_t_errorf_v(
                t, "%s(%q) = %d.%d %q %d in %q, %q, want %d.%d %q %d in %q, %q", what,
                c->input, time_unix(tm), time_nanosecond(tm), zn, off, ln, err_str(err),
                c->unix, c->nsec, c->zone, c->off, c->locname, want_err(c->err));
        error_release(m);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void TestParseTimeZone(TestingT *t) {
    for (size_t i = 0; i < LEN(g_tz_names); i++) {
        const ZCase *c = &g_tz_names[i];
        Int n = -1;
        bool ok = burrow__time_parse_time_zone(cstr(c->value), &n);
        if (ok != (c->ok != 0) || n != c->length)
            testing_t_errorf_v(t, "parseTimeZone(%q) = %d %v, want %d %v", c->value, n,
                               ok, c->length, c->ok != 0);
    }
}

/* Not in the generated cases, since Go can not make a Time with a chosen
 * monotonic reading from outside the package: String's m= suffix. */
static void TestStringMonotonic(TestingT *t) {
    load_locs(t);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Time n = time_now();
    if ((n.wall >> 63) == 0) {
        testing_t_skip_v(t, "time_now has no monotonic reading here");
        arena_free(&ar);
        return;
    }
    Str base = time_string(time_round(n, 0), a);
    static const struct {
        int64_t ext;
        const char *suffix;
    } cases[] = {
        {INT64_C(1000000001), " m=+1.000000001"},
        {INT64_C(-5), " m=-0.000000005"},
        {INT64_C(0), " m=+0.000000000"},
        {INT64_C(1000000000000000000), " m=+1000000000.000000000"},
        {INT64_C(-1234567890123456789), " m=-1234567890.123456789"},
        {INT64_MIN, " m=-9223372036.854775808"},
    };
    for (size_t i = 0; i < LEN(cases); i++) {
        Time m = n;
        m.ext = cases[i].ext;
        Str got = time_string(m, a);
        Str sfx = cstr(cases[i].suffix);
        if (got.len != base.len + sfx.len ||
            memcmp(got.p, base.p, (size_t)base.len) != 0 ||
            memcmp(got.p + base.len, sfx.p, (size_t)sfx.len) != 0)
            testing_t_errorf_v(t, "String() = %q, want %q%q", got, base, sfx);
    }
    arena_free(&ar);
}

/* Not in Go: the descriptor, as fmt and the encoders see it. */
static void TestDescriptor(TestingT *t) {
    load_locs(t);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CHECK(type_methods_sorted(TYPE_TIME));
    CHECK_INT_EQ(TYPE_TIME->nmethod, 13);
    CHECK(type_method_by_name(TYPE_TIME, BURROW_S("String")) != NULL);
    CHECK(type_method_by_name(TYPE_TIME, BURROW_S("IsZero")) != NULL);
    CHECK(type_method_by_name(TYPE_TIME, BURROW_S("MarshalJSON")) != NULL);
    CHECK(type_method_by_name(TYPE_TIME, BURROW_S("Format")) == NULL);

    Time tm = time_date(2009, TIME_NOVEMBER, 10, 23, 0, 0, 5, time_utc_loc);
    Str s = fmt_sprintf_v(a, "%v|%s|%#v", BURROW_ANY(TYPE_TIME, &tm),
                          BURROW_ANY(TYPE_TIME, &tm), BURROW_ANY(TYPE_TIME, &tm));
    CHECK(str_eq(s, cstr("2009-11-10 23:00:00.000000005 +0000 UTC|"
                         "2009-11-10 23:00:00.000000005 +0000 UTC|"
                         "time.Date(2009, time.November, 10, 23, 0, 0, 5, time.UTC)")));

    Error err = BURROW_NO_ERROR;
    Slice j = json_marshal(a, BURROW_ANY(TYPE_TIME, &tm), &err);
    CHECK(!BURROW_FAILED(err));
    CHECK(str_eq(((Str){j.p, j.len}), cstr("\"2009-11-10T23:00:00.000000005Z\"")));
    Time back = {0, 0, NULL};
    err = json_unmarshal(a, j, BURROW_ANY(TYPE_TIME, &back));
    CHECK(!BURROW_FAILED(err));
    CHECK(time_equal(back, tm));
    arena_free(&ar);
}

/* A layout long enough to go past the buffer on the stack. */
static void TestFormatLong(TestingT *t) {
    load_locs(t);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Time tm = time_date(2009, TIME_FEBRUARY, 4, 21, 0, 57, 12345600, time_local_loc);
    Byte layout[1200];
    Byte want[4000];
    Int ln = 0;
    Int wn = 0;
    for (int i = 0; i < 100; i++) {
        memcpy(layout + ln, "Monday MST ", 11);
        ln += 11;
        memcpy(want + wn, "Wednesday PST ", 14);
        wn += 14;
    }
    Str got = time_format(tm, a, (Str){layout, ln});
    CHECK(str_eq(got, ((Str){want, wn})));
    Slice b = time_append_format(tm, a, slice_nil(TYPE_BYTE), (Str){layout, ln});
    CHECK(str_eq(((Str){b.p, b.len}), ((Str){want, wn})));
    Str gs = time_go_string(time_in(tm, g_locs[0]), a);
    CHECK(str_eq(gs, cstr("time.Date(2009, time.February, 4, 21, 0, 57, 12345600, "
                          "time.Location(\"America/Los_Angeles\"))")));
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestFormat)                                                                      \
    X(TestParseRoundTrip)                                                              \
    X(TestParse)                                                                       \
    X(TestStringsAndEncodings)                                                         \
    X(TestUnmarshal)                                                                   \
    X(TestParseTimeZone)                                                               \
    X(TestStringMonotonic)                                                             \
    X(TestDescriptor)                                                                  \
    X(TestFormatLong)

TESTING_MAIN(TESTS)
