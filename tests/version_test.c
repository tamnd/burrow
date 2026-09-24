/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/burrow.h"
#include "check.h"

static void TestVersionStringMatchesTheComponents(TestingT *t) {
    char want[32];
    snprintf(want, sizeof want, "%d.%d.%d", BURROW_VERSION_MAJOR, BURROW_VERSION_MINOR,
             BURROW_VERSION_PATCH);
    CHECK_STR_EQ(BURROW_VERSION_STRING, want);
    CHECK_STR_EQ(burrow_version(), want);
}

/* The header you compiled against and the library you linked against can
 * disagree if somebody ships burrow as a shared object. In this build they
 * cannot, and the test exists so that the day they can, it catches it. */
static void TestVersionNumberMatchesTheString(TestingT *t) {
    CHECK_INT_EQ(burrow_version_number(), BURROW_VERSION_NUMBER);
    CHECK_INT_EQ(BURROW_VERSION_NUMBER, BURROW_VERSION_MAJOR * 10000 +
                                            BURROW_VERSION_MINOR * 100 +
                                            BURROW_VERSION_PATCH);
}

static void TestProvenanceIsRecorded(TestingT *t) {
    CHECK(burrow_sourceid() != NULL);
    CHECK(strncmp(burrow_go_version(), "go1.", 4) == 0);
}

/* The attribution clause is satisfied by a binary that can print this, so it
 * has to actually be in there and it has to say the things the licence and the
 * trademark position require. Losing any of these lines to a careless edit is a
 * legal problem rather than a cosmetic one, which is why it is a test. */
static void TestEmbeddedLicenceCarriesTheRequiredNotices(TestingT *t) {
    const char *l = burrow_license();
    CHECK(strstr(l, "Copyright 2009 The Go Authors") != NULL);
    CHECK(strstr(l, "Copyright 2026 The burrow Authors") != NULL);
    CHECK(strstr(l, "not affiliated with, endorsed by, or sponsored by") != NULL);
    CHECK(strstr(l, "Additional IP Rights Grant (Patents)") != NULL);
    CHECK(strstr(l, "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS") != NULL);
}

#define TESTS(X)                                                                       \
    X(TestVersionStringMatchesTheComponents)                                           \
    X(TestVersionNumberMatchesTheString)                                               \
    X(TestProvenanceIsRecorded)                                                        \
    X(TestEmbeddedLicenceCarriesTheRequiredNotices)

TESTING_MAIN(TESTS)
