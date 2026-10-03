/* Derived from Go's src/time/tzdata_test.go.
 * Go source: go1.27.1.
 *
 * TestEmbeddedTZData is Go's. The tests after it are not in Go: every zone in
 * the zip read through archive/zip and through tzdata's own reader must be
 * the same bytes and must load, a name the database does not have is not an
 * error until nothing else has it either, and registering twice is the same
 * as once.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/time/time_internal.h"

#include "burrow/archive/zip.h"
#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/time.h"
#include "burrow/time/tzdata.h"

#include <string.h>

/* The zip itself, from tzdata_zip.c. */
extern const Str burrow__tzdata_zip;

static const char *const zones[] = {
    "Asia/Jerusalem",
    "America/Los_Angeles",
};

/* Go compares the name and zone fields of the two Locations by reflection.
 * The tx field changes faster as tzdata is updated, and the cache fields are
 * expected to differ, so those are left alone the same way. */
static bool same_zones(TimeLocation *a, TimeLocation *b) {
    if (!str_eq(a->name, b->name) || a->nzone != b->nzone)
        return false;
    for (Int i = 0; i < a->nzone; i++) {
        if (!str_eq(a->zone[i].name, b->zone[i].name) ||
            a->zone[i].offset != b->zone[i].offset ||
            a->zone[i].is_dst != b->zone[i].is_dst)
            return false;
    }
    return true;
}

static void TestEmbeddedTZData(TestingT *t) {
    tzdata_register();
    burrow__time_disable_platform_sources(true);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    for (size_t i = 0; i < sizeof zones / sizeof zones[0]; i++) {
        Str zone = str_from_cstr(zones[i]);
        Error err = BURROW_NO_ERROR;
        TimeLocation *ref = time_load_location(zone, &err);
        if (ref == NULL) {
            testing_t_errorf_v(t, "LoadLocation(%q): %v", zone, err);
            continue;
        }

        Str embedded = {NULL, 0};
        if (!burrow__time_load_from_embedded(zone, &embedded, &err)) {
            testing_t_errorf_v(t, "LoadFromEmbeddedTZData(%q): %v", zone, err);
            continue;
        }
        TimeLocation *sample = time_load_location_from_tz_data(
            a, zone,
            (Slice){(void *)(uintptr_t)embedded.p, embedded.len, embedded.len,
                    TYPE_BYTE},
            &err);
        if (sample == NULL) {
            testing_t_errorf_v(t, "LoadLocationFromTZData failed for %q: %v", zone,
                               err);
            continue;
        }

        if (!same_zones(ref, sample))
            testing_t_errorf_v(
                t, "zone %s: system and embedded tzdata field zone differs", zone);
    }
    burrow__time_disable_platform_sources(false);
    arena_free(&ar);
}

/* Every entry in the zip, found by archive/zip, is what tzdata hands back for
 * its name, and is a zone time can load. */
static void TestEmbeddedEveryZone(TestingT *t) {
    tzdata_register();
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Slice zip = {(void *)(uintptr_t)burrow__tzdata_zip.p, burrow__tzdata_zip.len,
                 burrow__tzdata_zip.len, TYPE_BYTE};
    BytesReader br;
    bytes_reader_reset(&br, zip);
    ZipReader *zr = zip_new_reader(a, bytes_reader_as_io_reader_at(&br), zip.len, &err);
    if (zr == NULL)
        testing_t_fatalf_v(t, "zip.NewReader: %v", err);
    if (zr->file.len < 300)
        testing_t_errorf_v(t, "%d zones in the zip, want hundreds", zr->file.len);

    Int checked = 0;
    for (Int i = 0; i < zr->file.len; i++) {
        ZipFile *zf = ((ZipFile **)zr->file.p)[i];
        Str name = zf->file_header.name;
        IoReadCloser rc = zip_file_open(zf, a, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: Open: %v", name, err);
            continue;
        }
        Slice want = io_read_all(a, (IoReader){&rc.vt->reader, rc.data}, &err);
        (void)rc.vt->closer.close(rc.data);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: ReadAll: %v", name, err);
            continue;
        }

        Str got = {NULL, 0};
        if (!burrow__time_load_from_embedded(name, &got, &err)) {
            testing_t_errorf_v(t, "LoadFromEmbeddedTZData(%q): %v", name, err);
            continue;
        }
        if (got.len != want.len || memcmp(got.p, want.p, (size_t)got.len) != 0) {
            testing_t_errorf_v(t, "%s: %d bytes from tzdata, %d from archive/zip", name,
                               got.len, want.len);
            continue;
        }
        if (time_load_location_from_tz_data(a, name, want, &err) == NULL)
            testing_t_errorf_v(t, "LoadLocationFromTZData(%q): %v", name, err);
        checked++;
    }
    testing_t_logf_v(t, "%d zones", checked);
    arena_free(&ar);
}

/* A name the database does not have is not found, with no error, which is
 * Go's ENOENT. LoadLocation with nothing else to look at then says the zone is
 * unknown. */
static void TestEmbeddedNotFound(TestingT *t) {
    tzdata_register();
    Error err = BURROW_NO_ERROR;
    Str data = {NULL, 0};
    CHECK(!burrow__time_load_from_embedded(BURROW_S("Mars/Olympus_Mons"), &data, &err));
    CHECK(BURROW_OK(err));
    CHECK(!burrow__time_load_from_embedded(BURROW_S(""), &data, &err));
    CHECK(BURROW_OK(err));

    burrow__time_disable_platform_sources(true);
    CHECK(time_load_location(BURROW_S("Mars/Olympus_Mons"), &err) == NULL);
    Str want = BURROW_S("unknown time zone Mars/Olympus_Mons");
    if (!str_eq(error_text(err), want))
        testing_t_errorf_v(t, "got %q, want %q", error_text(err), want);
    burrow__time_disable_platform_sources(false);
}

/* With only the built-in database to go on, a zone loads and says what Go
 * says about it, and a second load is the first one again. */
static void TestEmbeddedOnly(TestingT *t) {
    tzdata_register();
    tzdata_register();
    burrow__time_disable_platform_sources(true);
    Error err = BURROW_NO_ERROR;
    TimeLocation *l = time_load_location(BURROW_S("Europe/Berlin"), &err);
    burrow__time_disable_platform_sources(false);
    if (l == NULL)
        testing_t_fatalf_v(t, "LoadLocation: %v", err);
    CHECK(time_load_location(BURROW_S("Europe/Berlin"), &err) == l);
    CHECK(str_eq(time_location_string(l), BURROW_S("Europe/Berlin")));

    /* 2001-09-09T01:46:40Z in summer and 2001-01-01T00:00:00Z in winter. */
    Int off = 0;
    Str name = time_zone(time_in(time_from_unix(1000000000, 0), l), &off);
    CHECK(str_eq(name, BURROW_S("CEST")));
    CHECK_INT_EQ(off, 2 * 3600);
    name = time_zone(time_in(time_from_unix(978307200, 0), l), &off);
    CHECK(str_eq(name, BURROW_S("CET")));
    CHECK_INT_EQ(off, 3600);
}

#define TESTS(X)                                                                       \
    X(TestEmbeddedTZData)                                                              \
    X(TestEmbeddedEveryZone)                                                           \
    X(TestEmbeddedNotFound)                                                            \
    X(TestEmbeddedOnly)
TESTING_MAIN(TESTS)
