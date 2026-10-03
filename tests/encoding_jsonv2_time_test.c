/* Derived from Go's src/encoding/json/v2/arshal_time_test.go.
 * Go source: go1.27.1.
 *
 * The tables are Go's, written out by a Go program that ran them through
 * go1.27.1. The fuzz tests are not ported.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/encoding/jsonv2_internal.h"

#include "burrow/declare.h"
#include "burrow/encoding/json.h"
#include "burrow/encoding/json/v2.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/strconv.h"
#include "burrow/time.h"

#include <stdint.h>
#include <string.h>

enum { ERR_SYNTAX = 1, ERR_RANGE, ERR_INACCURATE };

typedef struct FormatDurationCase {
    int64_t td;
    const char *base10_sec, *base10_milli, *base10_micro, *base10_nano, *iso8601;
} FormatDurationCase;

typedef struct ParseDurationCase {
    const char *in;
    uint64_t base;
    int64_t want;
    int want_err;
} ParseDurationCase;

typedef struct FormatTimeCase {
    int64_t sec, nsec;
    const char *unix_sec, *unix_milli, *unix_micro, *unix_nano;
} FormatTimeCase;

typedef struct ParseTimeCase {
    const char *in;
    uint64_t base;
    bool zero;
    int64_t sec, nsec;
    int want_err;
} ParseTimeCase;

static const FormatDurationCase format_duration_testdata[] = {
    {INT64_C(9223372036854775807), "9223372036.854775807", "9223372036854.775807",
     "9223372036854775.807", "9223372036854775807", "PT2562047H47M16.854775807S"},
    {INT64_C(443096000000000), "443096", "443096000", "443096000000", "443096000000000",
     "PT123H4M56S"},
    {INT64_C(3600000000000), "3600", "3600000", "3600000000", "3600000000000", "PT1H"},
    {INT64_C(60000000000), "60", "60000", "60000000", "60000000000", "PT1M"},
    {INT64_C(2000000000000), "2000", "2000000", "2000000000", "2000000000000",
     "PT33M20S"},
    {INT64_C(1100000000000), "1100", "1100000", "1100000000", "1100000000000",
     "PT18M20S"},
    {INT64_C(1010000000000), "1010", "1010000", "1010000000", "1010000000000",
     "PT16M50S"},
    {INT64_C(1001000000000), "1001", "1001000", "1001000000", "1001000000000",
     "PT16M41S"},
    {INT64_C(1000100000000), "1000.1", "1000100", "1000100000", "1000100000000",
     "PT16M40.1S"},
    {INT64_C(1000010000000), "1000.01", "1000010", "1000010000", "1000010000000",
     "PT16M40.01S"},
    {INT64_C(1000001000000), "1000.001", "1000001", "1000001000", "1000001000000",
     "PT16M40.001S"},
    {INT64_C(1000000100000), "1000.0001", "1000000.1", "1000000100", "1000000100000",
     "PT16M40.0001S"},
    {INT64_C(1000000010000), "1000.00001", "1000000.01", "1000000010", "1000000010000",
     "PT16M40.00001S"},
    {INT64_C(1000000001000), "1000.000001", "1000000.001", "1000000001",
     "1000000001000", "PT16M40.000001S"},
    {INT64_C(1000000000100), "1000.0000001", "1000000.0001", "1000000000.1",
     "1000000000100", "PT16M40.0000001S"},
    {INT64_C(1000000000010), "1000.00000001", "1000000.00001", "1000000000.01",
     "1000000000010", "PT16M40.00000001S"},
    {INT64_C(1000000000001), "1000.000000001", "1000000.000001", "1000000000.001",
     "1000000000001", "PT16M40.000000001S"},
    {INT64_C(1000000001), "1.000000001", "1000.000001", "1000000.001", "1000000001",
     "PT1.000000001S"},
    {INT64_C(1000000000), "1", "1000", "1000000", "1000000000", "PT1S"},
    {INT64_C(999999999), "0.999999999", "999.999999", "999999.999", "999999999",
     "PT0.999999999S"},
    {INT64_C(100000000), "0.1", "100", "100000", "100000000", "PT0.1S"},
    {INT64_C(120000000), "0.12", "120", "120000", "120000000", "PT0.12S"},
    {INT64_C(123000000), "0.123", "123", "123000", "123000000", "PT0.123S"},
    {INT64_C(123400000), "0.1234", "123.4", "123400", "123400000", "PT0.1234S"},
    {INT64_C(123450000), "0.12345", "123.45", "123450", "123450000", "PT0.12345S"},
    {INT64_C(123456000), "0.123456", "123.456", "123456", "123456000", "PT0.123456S"},
    {INT64_C(123456700), "0.1234567", "123.4567", "123456.7", "123456700",
     "PT0.1234567S"},
    {INT64_C(123456780), "0.12345678", "123.45678", "123456.78", "123456780",
     "PT0.12345678S"},
    {INT64_C(123456789), "0.123456789", "123.456789", "123456.789", "123456789",
     "PT0.123456789S"},
    {INT64_C(12345678), "0.012345678", "12.345678", "12345.678", "12345678",
     "PT0.012345678S"},
    {INT64_C(1234567), "0.001234567", "1.234567", "1234.567", "1234567",
     "PT0.001234567S"},
    {INT64_C(123456), "0.000123456", "0.123456", "123.456", "123456", "PT0.000123456S"},
    {INT64_C(12345), "0.000012345", "0.012345", "12.345", "12345", "PT0.000012345S"},
    {INT64_C(1234), "0.000001234", "0.001234", "1.234", "1234", "PT0.000001234S"},
    {INT64_C(123), "0.000000123", "0.000123", "0.123", "123", "PT0.000000123S"},
    {INT64_C(12), "0.000000012", "0.000012", "0.012", "12", "PT0.000000012S"},
    {INT64_C(1), "0.000000001", "0.000001", "0.001", "1", "PT0.000000001S"},
    {INT64_C(0), "0", "0", "0", "0", "PT0S"},
    {INT64_C(-1), "-0.000000001", "-0.000001", "-0.001", "-1", "-PT0.000000001S"},
    {INT64_C(-12), "-0.000000012", "-0.000012", "-0.012", "-12", "-PT0.000000012S"},
    {INT64_C(-123), "-0.000000123", "-0.000123", "-0.123", "-123", "-PT0.000000123S"},
    {INT64_C(-1234), "-0.000001234", "-0.001234", "-1.234", "-1234", "-PT0.000001234S"},
    {INT64_C(-12345), "-0.000012345", "-0.012345", "-12.345", "-12345",
     "-PT0.000012345S"},
    {INT64_C(-123456), "-0.000123456", "-0.123456", "-123.456", "-123456",
     "-PT0.000123456S"},
    {INT64_C(-1234567), "-0.001234567", "-1.234567", "-1234.567", "-1234567",
     "-PT0.001234567S"},
    {INT64_C(-12345678), "-0.012345678", "-12.345678", "-12345.678", "-12345678",
     "-PT0.012345678S"},
    {INT64_C(-123456789), "-0.123456789", "-123.456789", "-123456.789", "-123456789",
     "-PT0.123456789S"},
    {INT64_C(-123456780), "-0.12345678", "-123.45678", "-123456.78", "-123456780",
     "-PT0.12345678S"},
    {INT64_C(-123456700), "-0.1234567", "-123.4567", "-123456.7", "-123456700",
     "-PT0.1234567S"},
    {INT64_C(-123456000), "-0.123456", "-123.456", "-123456", "-123456000",
     "-PT0.123456S"},
    {INT64_C(-123450000), "-0.12345", "-123.45", "-123450", "-123450000",
     "-PT0.12345S"},
    {INT64_C(-123400000), "-0.1234", "-123.4", "-123400", "-123400000", "-PT0.1234S"},
    {INT64_C(-123000000), "-0.123", "-123", "-123000", "-123000000", "-PT0.123S"},
    {INT64_C(-120000000), "-0.12", "-120", "-120000", "-120000000", "-PT0.12S"},
    {INT64_C(-100000000), "-0.1", "-100", "-100000", "-100000000", "-PT0.1S"},
    {INT64_C(-999999999), "-0.999999999", "-999.999999", "-999999.999", "-999999999",
     "-PT0.999999999S"},
    {INT64_C(-1000000000), "-1", "-1000", "-1000000", "-1000000000", "-PT1S"},
    {INT64_C(-1000000001), "-1.000000001", "-1000.000001", "-1000000.001",
     "-1000000001", "-PT1.000000001S"},
    {INT64_MIN, "-9223372036.854775808", "-9223372036854.775808",
     "-9223372036854775.808", "-9223372036854775808", "-PT2562047H47M16.854775808S"},
};

static const ParseDurationCase parse_duration_testdata[] = {
    {"0", 1, INT64_C(0), 0},
    {"0.", 1, INT64_C(0), ERR_SYNTAX},
    {"0.0", 1, INT64_C(0), 0},
    {"0.00", 1, INT64_C(0), 0},
    {"00.0", 1, INT64_C(0), ERR_SYNTAX},
    {"+0", 1, INT64_C(0), ERR_SYNTAX},
    {"1e0", 1, INT64_C(0), ERR_SYNTAX},
    {"1.000000000x", 1000000000, INT64_C(0), ERR_SYNTAX},
    {"1.000000x", 1000000, INT64_C(0), ERR_SYNTAX},
    {"1.000x", 1000, INT64_C(0), ERR_SYNTAX},
    {"1.x", 1, INT64_C(0), ERR_SYNTAX},
    {"1.0000000009", 1000000000, INT64_C(1000000000), 0},
    {"1.0000009", 1000000, INT64_C(1000000), 0},
    {"1.0009", 1000, INT64_C(1000), 0},
    {"1.9", 1, INT64_C(1), 0},
    {"-9223372036854775809", 1, INT64_C(0), ERR_RANGE},
    {"9223372036854775.808", 1000, INT64_C(0), ERR_RANGE},
    {"-9223372036854.775809", 1000000, INT64_C(0), ERR_RANGE},
    {"9223372036.854775808", 1000000000, INT64_C(0), ERR_RANGE},
    {"-1.9", 1, INT64_C(-1), 0},
    {"-1.0009", 1000, INT64_C(-1000), 0},
    {"-1.0000009", 1000000, INT64_C(-1000000), 0},
    {"-1.0000000009", 1000000000, INT64_C(-1000000000), 0},
    {"", 8601, INT64_C(0), ERR_SYNTAX},
    {"P", 8601, INT64_C(0), ERR_SYNTAX},
    {"PT", 8601, INT64_C(0), ERR_SYNTAX},
    {"PT0", 8601, INT64_C(0), ERR_SYNTAX},
    {"DT0S", 8601, INT64_C(0), ERR_SYNTAX},
    {"PT0S", 8601, INT64_C(0), 0},
    {" PT0S", 8601, INT64_C(0), ERR_SYNTAX},
    {"PT0S ", 8601, INT64_C(0), ERR_SYNTAX},
    {"+PT0S", 8601, INT64_C(0), 0},
    {"PT0.M", 8601, INT64_C(0), ERR_SYNTAX},
    {"PT0.S", 8601, INT64_C(0), ERR_SYNTAX},
    {"PT0.0S", 8601, INT64_C(0), 0},
    {"PT0.0_0H", 8601, INT64_C(0), ERR_SYNTAX},
    {"PT0.0_0M", 8601, INT64_C(0), ERR_SYNTAX},
    {"PT0.0_0S", 8601, INT64_C(0), ERR_SYNTAX},
    {"PT.0S", 8601, INT64_C(0), ERR_SYNTAX},
    {"PT00.0S", 8601, INT64_C(0), 0},
    {"PT0S", 8601, INT64_C(0), 0},
    {"PT1,5S", 8601, INT64_C(1500000000), 0},
    {"PT1H", 8601, INT64_C(3600000000000), 0},
    {"PT1H0S", 8601, INT64_C(3600000000000), 0},
    {"PT0S", 8601, INT64_C(0), 0},
    {"PT00S", 8601, INT64_C(0), 0},
    {"PT000S", 8601, INT64_C(0), 0},
    {"PTS", 8601, INT64_C(0), ERR_SYNTAX},
    {"PT1M", 8601, INT64_C(60000000000), 0},
    {"PT01M", 8601, INT64_C(60000000000), 0},
    {"PT001M", 8601, INT64_C(60000000000), 0},
    {"PT1H59S", 8601, INT64_C(3659000000000), 0},
    {"PT123H4M56.789S", 8601, INT64_C(443096789000000), 0},
    {"-PT123H4M56.789S", 8601, INT64_C(-443096789000000), 0},
    {"PT0H0S", 8601, INT64_C(0), 0},
    {"PT0H", 8601, INT64_C(0), 0},
    {"PT0M", 8601, INT64_C(0), 0},
    {"-PT0S", 8601, INT64_C(0), 0},
    {"PT1M0S", 8601, INT64_C(60000000000), 0},
    {"PT0H1M0S", 8601, INT64_C(60000000000), 0},
    {"PT01H02M03S", 8601, INT64_C(3723000000000), 0},
    {"PT0,123S", 8601, INT64_C(123000000), 0},
    {"PT1.S", 8601, INT64_C(0), ERR_SYNTAX},
    {"PT1.000S", 8601, INT64_C(1000000000), 0},
    {"PT0.025H", 8601, INT64_C(90000000000), 0},
    {"PT0.025H0M", 8601, INT64_C(0), ERR_SYNTAX},
    {"PT1.5M", 8601, INT64_C(90000000000), 0},
    {"PT1.5M0S", 8601, INT64_C(0), ERR_SYNTAX},
    {"PT60M", 8601, INT64_C(3600000000000), 0},
    {"PT3600S", 8601, INT64_C(3600000000000), 0},
    {"PT1H2M3.0S", 8601, INT64_C(3723000000000), 0},
    {"pt1h2m3,0s", 8601, INT64_C(3723000000000), 0},
    {"PT-1H-2M-3S", 8601, INT64_C(0), ERR_SYNTAX},
    {"P1Y", 8601, INT64_C(31556952000000000), ERR_INACCURATE},
    {"P1.0Y", 8601, INT64_C(0), ERR_SYNTAX},
    {"P1M", 8601, INT64_C(2629746000000000), ERR_INACCURATE},
    {"P1.0M", 8601, INT64_C(0), ERR_SYNTAX},
    {"P1W", 8601, INT64_C(604800000000000), ERR_INACCURATE},
    {"P1.0W", 8601, INT64_C(0), ERR_SYNTAX},
    {"P1D", 8601, INT64_C(86400000000000), ERR_INACCURATE},
    {"P1.0D", 8601, INT64_C(0), ERR_SYNTAX},
    {"P1W1S", 8601, INT64_C(0), ERR_SYNTAX},
    {"-P1Y2M3W4DT5H6M7.8S", 8601, INT64_C(-38994811800000000), ERR_INACCURATE},
    {"-p1y2m3w4dt5h6m7.8s", 8601, INT64_C(-38994811800000000), ERR_INACCURATE},
    {"P0Y0M0DT1H2M3S", 8601, INT64_C(3723000000000), ERR_INACCURATE},
    {"PT0.0000000001S", 8601, INT64_C(0), 0},
    {"PT0.0000000005S", 8601, INT64_C(0), 0},
    {"PT0.000000000500000000S", 8601, INT64_C(0), 0},
    {"PT0.000000000499999999S", 8601, INT64_C(0), 0},
    {"PT2562047H47M16.854775808S", 8601, INT64_C(0), ERR_RANGE},
    {"-PT2562047H47M16.854775809S", 8601, INT64_C(0), ERR_RANGE},
    {"PT9223372036.854775807S", 8601, INT64_C(9223372036854775807), 0},
    {"PT9223372036.854775808S", 8601, INT64_C(0), ERR_RANGE},
    {"-PT9223372036.854775808S", 8601, INT64_MIN, 0},
    {"-PT9223372036.854775809S", 8601, INT64_C(0), ERR_RANGE},
    {"PT18446744073709551616S", 8601, INT64_C(0), ERR_RANGE},
    {"PT5124096H", 8601, INT64_C(0), ERR_RANGE},
    {"PT2562047.7880152155019444H", 8601, INT64_C(9223372036854775807), 0},
    {"PT2562047.7880152155022222H", 8601, INT64_C(0), ERR_RANGE},
    {"PT5124094H94M33.709551616S", 8601, INT64_C(0), ERR_RANGE},
};

static const FormatTimeCase format_time_testdata[] = {
    {INT64_C(9223372036854775807), 999999999, "9223372036854775807.999999999",
     "9223372036854775807999.999999", "9223372036854775807999999.999",
     "9223372036854775807999999999"},
    {INT64_C(922337203685477580), 999999999, "922337203685477580.999999999",
     "922337203685477580999.999999", "922337203685477580999999.999",
     "922337203685477580999999999"},
    {INT64_C(92233720368547758), 999999999, "92233720368547758.999999999",
     "92233720368547758999.999999", "92233720368547758999999.999",
     "92233720368547758999999999"},
    {INT64_MIN, 1, "-9223372036854775807.999999999", "-9223372036854775807999.999999",
     "-9223372036854775807999999.999", "-9223372036854775807999999999"},
    {INT64_MIN, 0, "-9223372036854775808", "-9223372036854775808000",
     "-9223372036854775808000000", "-9223372036854775808000000000"},
    {INT64_C(9223372036), 854775807, "9223372036.854775807", "9223372036854.775807",
     "9223372036854775.807", "9223372036854775807"},
    {INT64_C(443096), 0, "443096", "443096000", "443096000000", "443096000000000"},
    {INT64_C(3600), 0, "3600", "3600000", "3600000000", "3600000000000"},
    {INT64_C(60), 0, "60", "60000", "60000000", "60000000000"},
    {INT64_C(2000), 0, "2000", "2000000", "2000000000", "2000000000000"},
    {INT64_C(1100), 0, "1100", "1100000", "1100000000", "1100000000000"},
    {INT64_C(1010), 0, "1010", "1010000", "1010000000", "1010000000000"},
    {INT64_C(1001), 0, "1001", "1001000", "1001000000", "1001000000000"},
    {INT64_C(1000), 100000000, "1000.1", "1000100", "1000100000", "1000100000000"},
    {INT64_C(1000), 10000000, "1000.01", "1000010", "1000010000", "1000010000000"},
    {INT64_C(1000), 1000000, "1000.001", "1000001", "1000001000", "1000001000000"},
    {INT64_C(1000), 100000, "1000.0001", "1000000.1", "1000000100", "1000000100000"},
    {INT64_C(1000), 10000, "1000.00001", "1000000.01", "1000000010", "1000000010000"},
    {INT64_C(1000), 1000, "1000.000001", "1000000.001", "1000000001", "1000000001000"},
    {INT64_C(1000), 100, "1000.0000001", "1000000.0001", "1000000000.1",
     "1000000000100"},
    {INT64_C(1000), 10, "1000.00000001", "1000000.00001", "1000000000.01",
     "1000000000010"},
    {INT64_C(1000), 1, "1000.000000001", "1000000.000001", "1000000000.001",
     "1000000000001"},
    {INT64_C(1), 1, "1.000000001", "1000.000001", "1000000.001", "1000000001"},
    {INT64_C(1), 0, "1", "1000", "1000000", "1000000000"},
    {INT64_C(0), 999999999, "0.999999999", "999.999999", "999999.999", "999999999"},
    {INT64_C(0), 100000000, "0.1", "100", "100000", "100000000"},
    {INT64_C(0), 120000000, "0.12", "120", "120000", "120000000"},
    {INT64_C(0), 123000000, "0.123", "123", "123000", "123000000"},
    {INT64_C(0), 123400000, "0.1234", "123.4", "123400", "123400000"},
    {INT64_C(0), 123450000, "0.12345", "123.45", "123450", "123450000"},
    {INT64_C(0), 123456000, "0.123456", "123.456", "123456", "123456000"},
    {INT64_C(0), 123456700, "0.1234567", "123.4567", "123456.7", "123456700"},
    {INT64_C(0), 123456780, "0.12345678", "123.45678", "123456.78", "123456780"},
    {INT64_C(0), 123456789, "0.123456789", "123.456789", "123456.789", "123456789"},
    {INT64_C(0), 12345678, "0.012345678", "12.345678", "12345.678", "12345678"},
    {INT64_C(0), 1234567, "0.001234567", "1.234567", "1234.567", "1234567"},
    {INT64_C(0), 123456, "0.000123456", "0.123456", "123.456", "123456"},
    {INT64_C(0), 12345, "0.000012345", "0.012345", "12.345", "12345"},
    {INT64_C(0), 1234, "0.000001234", "0.001234", "1.234", "1234"},
    {INT64_C(0), 123, "0.000000123", "0.000123", "0.123", "123"},
    {INT64_C(0), 12, "0.000000012", "0.000012", "0.012", "12"},
    {INT64_C(0), 1, "0.000000001", "0.000001", "0.001", "1"},
    {INT64_C(0), 0, "0", "0", "0", "0"},
    {INT64_C(-1), 999999999, "-0.000000001", "-0.000001", "-0.001", "-1"},
    {INT64_C(-1), 999999988, "-0.000000012", "-0.000012", "-0.012", "-12"},
    {INT64_C(-1), 999999877, "-0.000000123", "-0.000123", "-0.123", "-123"},
    {INT64_C(-1), 999998766, "-0.000001234", "-0.001234", "-1.234", "-1234"},
    {INT64_C(-1), 999987655, "-0.000012345", "-0.012345", "-12.345", "-12345"},
    {INT64_C(-1), 999876544, "-0.000123456", "-0.123456", "-123.456", "-123456"},
    {INT64_C(-1), 998765433, "-0.001234567", "-1.234567", "-1234.567", "-1234567"},
    {INT64_C(-1), 987654322, "-0.012345678", "-12.345678", "-12345.678", "-12345678"},
    {INT64_C(-1), 876543211, "-0.123456789", "-123.456789", "-123456.789",
     "-123456789"},
    {INT64_C(-1), 876543220, "-0.12345678", "-123.45678", "-123456.78", "-123456780"},
    {INT64_C(-1), 876543300, "-0.1234567", "-123.4567", "-123456.7", "-123456700"},
    {INT64_C(-1), 876544000, "-0.123456", "-123.456", "-123456", "-123456000"},
    {INT64_C(-1), 876550000, "-0.12345", "-123.45", "-123450", "-123450000"},
    {INT64_C(-1), 876600000, "-0.1234", "-123.4", "-123400", "-123400000"},
    {INT64_C(-1), 877000000, "-0.123", "-123", "-123000", "-123000000"},
    {INT64_C(-1), 880000000, "-0.12", "-120", "-120000", "-120000000"},
    {INT64_C(-1), 900000000, "-0.1", "-100", "-100000", "-100000000"},
    {INT64_C(-1), 1, "-0.999999999", "-999.999999", "-999999.999", "-999999999"},
    {INT64_C(-1), 0, "-1", "-1000", "-1000000", "-1000000000"},
    {INT64_C(-2), 999999999, "-1.000000001", "-1000.000001", "-1000000.001",
     "-1000000001"},
    {INT64_C(-9223372037), 145224192, "-9223372036.854775808", "-9223372036854.775808",
     "-9223372036854775.808", "-9223372036854775808"},
};

static const ParseTimeCase parse_time_testdata[] = {
    {"0", 1, false, INT64_C(0), 0, 0},
    {"0.", 1, true, INT64_C(-62135596800), 0, ERR_SYNTAX},
    {"0.0", 1, false, INT64_C(0), 0, 0},
    {"0.00", 1, false, INT64_C(0), 0, 0},
    {"00.0", 1, true, INT64_C(-62135596800), 0, ERR_SYNTAX},
    {"+0", 1, true, INT64_C(-62135596800), 0, ERR_SYNTAX},
    {"1e0", 1, true, INT64_C(-62135596800), 0, ERR_SYNTAX},
    {"1234567890123456789012345678901234567890", 1, true, INT64_C(-62135596800), 0,
     ERR_RANGE},
    {"9223372036854775808000.000000", 1000, true, INT64_C(-62135596800), 0, ERR_RANGE},
    {"9223372036854775807999999.9999", 1000000, false, INT64_C(9223372036854775807),
     999999999, 0},
    {"9223372036854775807999999999.9", 1000000000, false, INT64_C(9223372036854775807),
     999999999, 0},
    {"9223372036854775807.999999999x", 1, true, INT64_C(-62135596800), 0, ERR_SYNTAX},
    {"9223372036854775807000000000", 1000000000, false, INT64_C(9223372036854775807), 0,
     0},
    {"-9223372036854775808", 1, false, INT64_MIN, 0, 0},
    {"-9223372036854775808000.000001", 1000, true, INT64_C(-62135596800), 0, ERR_RANGE},
    {"-9223372036854775808000000.0001", 1000000, false, INT64_MIN, 0, 0},
    {"-9223372036854775808000000000.x", 1000000000, true, INT64_C(-62135596800), 0,
     ERR_SYNTAX},
    {"-1234567890123456789012345678901234567890", 1000000000, true,
     INT64_C(-62135596800), 0, ERR_RANGE},
};

static Error want_error(int code) {
    switch (code) {
    case ERR_SYNTAX:
        return strconv_err_syntax;
    case ERR_RANGE:
        return strconv_err_range;
    case ERR_INACCURATE:
        return burrow__jsonv2_err_inaccurate_date_units;
    default:
        return BURROW_NO_ERROR;
    }
}

/* errors.Is(err, want), where a nil want only matches a nil err. */
static bool is_error(Error err, int code) {
    if (code == 0)
        return BURROW_OK(err);
    return errors_is(err, want_error(code));
}

static const char *base_label(uint64_t base, char *buf) {
    switch (base) {
    case 1:
        return "1e0";
    case 1000:
        return "1e3";
    case 1000000:
        return "1e6";
    case 1000000000:
        return "1e9";
    default:
        snprintf(buf, 24, "%llu", (unsigned long long)base);
        return buf;
    }
}

static Str buf_str(const JsonBuf *b) {
    return str_from_bytes(b->p, b->len);
}

/* Go's == on two Times, which compares the fields and not the instant. */
static bool same_time(Time a, Time b) {
    return a.wall == b.wall && a.ext == b.ext && a.loc == b.loc;
}

static void check_format_duration(TestingT *t, JsonBuf *b, int64_t td, const char *s,
                                  uint64_t base) {
    char lb[24];
    b->len = 0;
    burrow__jsonv2_append_duration(b, td, base);
    if (!str_eq(buf_str(b), str_from_cstr(s)))
        testing_t_errorf_v(t, "formatDuration(%d, %s) = %q, want %q", td,
                           base_label(base, lb), buf_str(b), s);
    Error err = BURROW_NO_ERROR;
    Duration got = burrow__jsonv2_parse_duration(buf_str(b), base, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "parseDuration(%q, %s) error: %v", buf_str(b),
                           base_label(base, lb), err);
    if (got != td)
        testing_t_errorf_v(t, "parseDuration(%q, %s) = %d, want %d", buf_str(b),
                           base_label(base, lb), got, td);
}

static void TestFormatDuration(TestingT *t) {
    JsonBuf b = {NULL, 0, 0, heap_allocator(), true, false};
    for (size_t i = 0;
         i < sizeof format_duration_testdata / sizeof format_duration_testdata[0];
         i++) {
        const FormatDurationCase *tt = &format_duration_testdata[i];
        check_format_duration(t, &b, tt->td, tt->base10_sec, 1000000000);
        check_format_duration(t, &b, tt->td, tt->base10_milli, 1000000);
        check_format_duration(t, &b, tt->td, tt->base10_micro, 1000);
        check_format_duration(t, &b, tt->td, tt->base10_nano, 1);
        check_format_duration(t, &b, tt->td, tt->iso8601, 8601);
    }
    burrow__jsonbuf_free(&b);
}

static void TestParseDuration(TestingT *t) {
    for (size_t i = 0;
         i < sizeof parse_duration_testdata / sizeof parse_duration_testdata[0]; i++) {
        const ParseDurationCase *tt = &parse_duration_testdata[i];
        char lb[24];
        Error err = BURROW_NO_ERROR;
        Duration td =
            burrow__jsonv2_parse_duration(str_from_cstr(tt->in), tt->base, &err);
        if (td != tt->want)
            testing_t_errorf_v(t, "parseDuration(%q, %s) = %d, want %d", tt->in,
                               base_label(tt->base, lb), td, tt->want);
        else if (!is_error(err, tt->want_err))
            testing_t_errorf_v(t, "parseDuration(%q, %s) error = %v, want %v", tt->in,
                               base_label(tt->base, lb), err, want_error(tt->want_err));
    }
}

static void check_format_time(TestingT *t, JsonBuf *b, Time ts, const char *s,
                              uint64_t pow10) {
    char lb[24];
    b->len = 0;
    burrow__jsonv2_append_time_unix(b, ts, pow10);
    if (!str_eq(buf_str(b), str_from_cstr(s)))
        testing_t_errorf_v(t, "formatTime(time.Unix(%d, %d), %s) = %q, want %q",
                           time_unix(ts), time_nanosecond(ts), base_label(pow10, lb),
                           buf_str(b), s);
    Error err = BURROW_NO_ERROR;
    Time got = burrow__jsonv2_parse_time_unix(buf_str(b), pow10, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "parseTime(%q, %s) error: %v", buf_str(b),
                           base_label(pow10, lb), err);
    if (!time_equal(got, ts))
        testing_t_errorf_v(
            t, "parseTime(%q, %s) = time.Unix(%d, %d), want time.Unix(%d, %d)",
            buf_str(b), base_label(pow10, lb), time_unix(got), time_nanosecond(got),
            time_unix(ts), time_nanosecond(ts));
}

static void TestFormatTime(TestingT *t) {
    JsonBuf b = {NULL, 0, 0, heap_allocator(), true, false};
    for (size_t i = 0; i < sizeof format_time_testdata / sizeof format_time_testdata[0];
         i++) {
        const FormatTimeCase *tt = &format_time_testdata[i];
        Time ts = time_utc(time_from_unix(tt->sec, tt->nsec));
        check_format_time(t, &b, ts, tt->unix_sec, 1);
        check_format_time(t, &b, ts, tt->unix_milli, 1000);
        check_format_time(t, &b, ts, tt->unix_micro, 1000000);
        check_format_time(t, &b, ts, tt->unix_nano, 1000000000);
    }
    burrow__jsonbuf_free(&b);
}

static void TestParseTime(TestingT *t) {
    for (size_t i = 0; i < sizeof parse_time_testdata / sizeof parse_time_testdata[0];
         i++) {
        const ParseTimeCase *tt = &parse_time_testdata[i];
        char lb[24];
        Time want = tt->zero ? (Time){0} : time_utc(time_from_unix(tt->sec, tt->nsec));
        Error err = BURROW_NO_ERROR;
        Time got =
            burrow__jsonv2_parse_time_unix(str_from_cstr(tt->in), tt->base, &err);
        if (!same_time(got, want))
            testing_t_errorf_v(
                t, "parseTime(%q, %s) = time.Unix(%d, %d), want time.Unix(%d, %d)",
                tt->in, base_label(tt->base, lb), time_unix(got), time_nanosecond(got),
                time_unix(want), time_nanosecond(want));
        else if (!is_error(err, tt->want_err))
            testing_t_errorf_v(t, "parseTime(%q, %s) error = %v, want %v", tt->in,
                               base_label(tt->base, lb), err, want_error(tt->want_err));
    }
}

/* Not in Go: the v1 API keeps encoding/json's old forms, a Duration as its
 * count of nanoseconds and a Time in RFC 3339, while v2 has no default form
 * for a Duration at all. */
static void TestV1Forms(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    Duration d = 90 * TIME_MINUTE;
    Slice b = json_marshal(a, BURROW_ANY(TYPE_DURATION, &d), &err);
    CHECK(!BURROW_FAILED(err));
    CHECK(str_eq(str_from_bytes(b.p, b.len), BURROW_S("5400000000000")));
    Duration back = 0;
    err = json_unmarshal(a, BURROW_B("-1500"), BURROW_ANY(TYPE_DURATION, &back));
    CHECK(!BURROW_FAILED(err));
    CHECK_INT_EQ(back, -1500);

    b = jsonv2_marshal_v(a, BURROW_ANY(TYPE_DURATION, &d), &err, 0);
    CHECK(BURROW_FAILED(err));

    Time tm = time_date(2026, TIME_OCTOBER, 3, 9, 30, 0, 5, time_utc_loc);
    b = json_marshal(a, BURROW_ANY(TYPE_TIME, &tm), &err);
    CHECK(!BURROW_FAILED(err));
    CHECK(str_eq(str_from_bytes(b.p, b.len),
                 BURROW_S("\"2026-10-03T09:30:00.000000005Z\"")));
    Time tb = {0, 0, NULL};
    err = json_unmarshal(a, b, BURROW_ANY(TYPE_TIME, &tb));
    CHECK(!BURROW_FAILED(err));
    CHECK(time_equal(tb, tm));
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestFormatDuration)                                                              \
    X(TestParseDuration)                                                               \
    X(TestFormatTime)                                                                  \
    X(TestParseTime)                                                                   \
    X(TestV1Forms)

TESTING_MAIN(TESTS)
