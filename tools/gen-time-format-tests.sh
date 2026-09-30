#!/bin/sh
# Regenerates tests/time_format_test_gen.h: what Go's Format, Parse, String,
# GoString and the text and JSON encodings of a Time say about a list of
# instants in a list of zones and a list of layouts.
#
# The zones are TZif files out of Go's lib/time/zoneinfo.zip, carried in the
# header as bytes. Go's own tests run with Local set to America/Los_Angeles
# under the name "Local", and this generator runs inside that test binary, so
# the C test makes its Local the same with burrow__time_force_local and gets
# the same answers wherever it runs.
#
# The formatted values in six of the locations are parsed again with Parse
# and ParseInLocation, and a few are parsed again after being cut short or
# having a byte changed at every position, which reaches most of the error
# paths. Go's own tables from
# format_test.go and time_test.go are added as they are.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/time_format_test_gen.h"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

goroot=$(go env GOROOT)
pkg=$goroot/src/time

cat > "$tmp/zz_gen_test.go" <<'GO'
package time_test

import (
	"archive/zip"
	"errors"
	"fmt"
	"io"
	"os"
	"runtime"
	"strings"
	"testing"
	. "time"
)

func loadTzinfoFromZip(zipfile, name string) ([]byte, error) {
	z, err := zip.OpenReader(zipfile)
	if err != nil {
		return nil, err
	}
	defer z.Close()
	f, err := z.Open(name)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	return io.ReadAll(f)
}

var genOut strings.Builder

func w(format string, args ...any) { fmt.Fprintf(&genOut, format, args...) }

// cq quotes s as a C string literal, every byte that is not plain printable
// ASCII as a three digit octal escape, which can not run into the next byte.
func cq(s string) string {
	var b strings.Builder
	b.WriteByte('"')
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == '"' || c == '\\' || c == '?':
			fmt.Fprintf(&b, "\\%03o", c)
		case c >= 0x20 && c < 0x7f:
			b.WriteByte(c)
		default:
			fmt.Fprintf(&b, "\\%03o", c)
		}
	}
	b.WriteByte('"')
	return b.String()
}

func cerr(err error) string {
	if err == nil {
		return "NULL"
	}
	return cq(err.Error())
}

func cbool(b bool) int {
	if b {
		return 1
	}
	return 0
}

type genZone struct {
	name string
	data []byte
	loc  *Location
}

type fixed struct {
	name string
	off  int
}

// The locations, numbered as the C test numbers them: parseOnly for Parse,
// which has no location, then Local, UTC, the zones and the fixed zones.
const (
	parseOnly = -3
	locLocal  = -2
	locUTC    = -1
)

var (
	gzones  []genZone
	fixeds = []fixed{
		{"", 3600}, {"", -(5*3600 + 30*60)}, {"XST", 5*3600 + 30*60 + 7}, {"", 45},
		{"", 24 * 3600}, {"", 123 * 3600}, {"", -(23*3600 + 59*60)}, {"ABCDE", 60},
		{"GMT+3", 3 * 3600},
	}
	fixedLocs []*Location
)

func locOf(i int) *Location {
	switch {
	case i == locLocal:
		return Local
	case i == locUTC:
		return UTC
	case i < len(gzones):
		return gzones[i].loc
	default:
		return fixedLocs[i-len(gzones)]
	}
}

// pc parses value with layout, with Parse when loc is parseOnly and with
// ParseInLocation otherwise, and writes the case and what Go made of it.
func pc(layout, value string, loc int) {
	var tm Time
	var err error
	if loc == parseOnly {
		tm, err = Parse(layout, value)
	} else {
		tm, err = ParseInLocation(layout, value, locOf(loc))
	}
	if err != nil {
		var pe *ParseError
		if !errors.As(err, &pe) {
			panic(err)
		}
		w("    {%s, %s, %d, %s, %s, %s, %s, INT64_C(0), 0, NULL, 0, NULL},\n", cq(layout), cq(value), loc, cerr(err), cq(pe.LayoutElem), cq(pe.ValueElem), cq(pe.Message))
		return
	}
	zn, zo := tm.Zone()
	w("    {%s, %s, %d, NULL, NULL, NULL, NULL, INT64_C(%d), %d, %s, %d, %s},\n", cq(layout), cq(value), loc, tm.Unix(), tm.Nanosecond(), cq(zn), zo, cq(tm.Location().String()))
}

// rc is pc for the value of format case fidx, which saves writing the layout
// and the value out again.
func rc(fidx int, layout, value string, loc int) {
	var tm Time
	var err error
	if loc == parseOnly {
		tm, err = Parse(layout, value)
	} else {
		tm, err = ParseInLocation(layout, value, locOf(loc))
	}
	if err != nil {
		w("    {%d, %d, %s, INT64_C(0), 0, NULL, 0, NULL},\n", fidx, loc, cerr(err))
		return
	}
	zn, zo := tm.Zone()
	w("    {%d, %d, NULL, INT64_C(%d), %d, %s, %d, %s},\n", fidx, loc, tm.Unix(), tm.Nanosecond(), cq(zn), zo, cq(tm.Location().String()))
}

func TestZZGen(t *testing.T) {
	zip := os.Getenv("GEN_ZIP")
	for _, name := range []string{
		"America/Los_Angeles", "America/New_York", "Europe/Berlin", "Asia/Kolkata",
		"Australia/Sydney", "Pacific/Chatham", "Europe/London", "America/Sao_Paulo",
		"Europe/Dublin", "Africa/Casablanca",
	} {
		data, err := loadTzinfoFromZip(zip, name)
		if err != nil {
			t.Fatal(err)
		}
		l, err := LoadLocationFromTZData(name, data)
		if err != nil {
			t.Fatal(err)
		}
		gzones = append(gzones, genZone{name: name, data: data, loc: l})
	}
	for _, f := range fixeds {
		fixedLocs = append(fixedLocs, FixedZone(f.name, f.off))
	}
	if Local.String() != "Local" {
		t.Fatal("Local is not the test's Los Angeles")
	}

	w("/* What Go's package time says about the cases of\n")
	w(" * tools/gen-time-format-tests.sh, %s. Generated by that script; do not\n", runtime.Version())
	w(" * edit. */\n\n")

	w("static const TzBlob g_zones[] = {\n")
	for _, z := range gzones {
		w("    {%q, %s, %d},\n", z.name, cq(string(z.data)), len(z.data))
	}
	w("};\n\n")
	w("static const TzFixed g_fixed[] = {\n")
	for _, f := range fixeds {
		w("    {%q, %d},\n", f.name, f.off)
	}
	w("};\n\n")

	layouts := []string{
		Layout, ANSIC, UnixDate, RubyDate, RFC822, RFC822Z, RFC850, RFC1123, RFC1123Z,
		RFC3339, RFC3339Nano, Kitchen, Stamp, StampMilli, StampMicro, StampNano,
		DateTime, DateOnly, TimeOnly,
		"2006-01-02T15:04:05.000Z07:00", "2006-01-02T15:04:05,000000Z0700",
		"2006-01-02 15:04:05Z070000", "2006-01-02 15:04Z07", "2006-01-02 15:04:05Z07:00:00",
		"2006-01-02 15:04:05 -070000", "2006-01-02 15:04:05 -07:00:00", "2006-01-02 15 -07",
		"2006 __2 002 15:04", "Jan _2 2006 15:04:05.9", "Jan _2 2006 15:04:05,999999",
		"3:04:05.00 pm", "03:04:05 PM", "Monday January 2 2006", "Mon Jan 02 06",
		"06 1 2 3 4 5", "15:04_2006 _2", "2006-002", "MST -0700 Z07:00",
		"15:04:05,999999999 MST", "Mon Jan 02 15:04:05.000000 MST 2006",
		"January Jan Monday Mon", "2006-01-02 15:04:05.999999999999",
		"Month Monday Janx", "no elements at all", "2006-01-02 1504 05 PM Z0700",
	}
	w("static const char *const g_layouts[] = {\n")
	for _, l := range layouts {
		w("    %s,\n", cq(l))
	}
	w("};\n\n")

	type inst struct{ sec, nsec int64 }
	insts := []inst{
		{0, 0}, {1233810057, 12345600}, {1e9, 123456789}, {-1, 999999999},
		{1700000000, 500000000}, {1710064800, 0}, {1730620800, 1}, {1730624400, 100000000},
		{-62135596800, 0}, {253402300800, 0}, {253402300799, 999999999}, {1e11, 0},
		{-1e11, 0}, {1700006400, 0}, {1700049600, 0}, {951782400, 20}, {-62167219200, 0},
		{-62198755200 + 3600, 5},
	}

	// Format in every location, and Parse and ParseInLocation of the result
	// in the locations in parseLocs.
	nloc := len(gzones) + len(fixeds)
	nz := len(gzones)
	formatLocs := map[int]bool{locLocal: true, locUTC: true, 1: true, 2: true, 3: true, 5: true, 6: true, 7: true, nz: true, nz + 2: true, nz + 3: true, nz + 4: true, nz + 8: true}
	parseLocs := map[int]bool{locLocal: true, locUTC: true, 3: true, 5: true, nz + 2: true, nz + 8: true}
	w("static const FCase g_formats[] = {\n")
	type rt struct {
		fidx          int
		layout, value string
		loc           int
	}
	var rts []rt
	fidx := 0
	for li := locLocal; li < nloc; li++ {
		if !formatLocs[li] {
			continue
		}
		loc := locOf(li)
		for _, in := range insts {
			tm := Unix(in.sec, in.nsec).In(loc)
			for lx, l := range layouts {
				s := tm.Format(l)
				w("    {%d, INT64_C(%d), %d, %d, %s},\n", li, in.sec, in.nsec, lx, cq(s))
				if parseLocs[li] {
					rts = append(rts, rt{fidx, l, s, li})
				}
				fidx++
			}
		}
	}
	w("};\n\n")

	w("static const RCase g_round_trips[] = {\n")
	for _, r := range rts {
		rc(r.fidx, r.layout, r.value, parseOnly)
		if r.loc != locLocal {
			rc(r.fidx, r.layout, r.value, r.loc)
		}
	}
	w("};\n\n")

	w("static const PCase g_parses[] = {\n")

	// Values cut short, with a byte gone, or with a byte replaced.
	for _, l := range []string{
		RFC3339, RFC3339Nano, RFC1123Z, UnixDate, RFC850, Kitchen, StampMicro,
		"2006-002 15:04:05.000 -07:00:00", "Jan _2 __2 06 3PM Z0700", ANSIC,
		"2006-01-02 15:04:05,999 MST",
	} {
		for _, in := range []inst{{1233810057, 12345600}} {
			v := Unix(in.sec, in.nsec).In(Local).Format(l)
			for i := 0; i < len(v); i++ {
				pc(l, v[:i], parseOnly)
				pc(l, v[:i]+v[i+1:], parseOnly)
				for _, c := range []byte{'9', 'x', '-', ' '} {
					if v[i] != c {
						pc(l, v[:i]+string(c)+v[i+1:], parseOnly)
					}
				}
			}
			pc(l, v+" ", parseOnly)
			pc(l, v+"\xff", parseOnly)
		}
	}

	// Go's tables.
	for _, tt := range parseTests {
		pc(tt.format, tt.value, parseOnly)
	}
	for _, tt := range rubyTests {
		pc(tt.format, tt.value, parseOnly)
	}
	for _, tt := range dayOutOfRangeTests {
		pc(ANSIC, tt.date, parseOnly)
	}
	for _, tt := range parseErrorTests {
		pc(tt.format, tt.value, parseOnly)
	}
	for _, tt := range secondsTimeZoneOffsetTests {
		pc(tt.format, tt.value, parseOnly)
	}
	for _, tt := range monthOutOfRangeTests {
		pc("01-02", tt.value, parseOnly)
	}
	for _, tt := range longFractionalDigitsTests {
		pc(RFC3339, tt.value, parseOnly)
		pc(RFC3339Nano, tt.value, parseOnly)
	}
	for i := 1; i <= 366; i++ {
		pc("2006-002", fmt.Sprintf("2020-%03d", i), parseOnly)
		pc("2006-__2", fmt.Sprintf("2019-%3d", i), parseOnly)
	}
	for _, v := range []string{"2006-01-02 002", "2006-01-02 __2"} {
		pc(v, "2020-02-29 060", parseOnly)
		pc(v, "2020-03-01 060", parseOnly)
		pc(v, "2020-02-28 060", parseOnly)
	}
	pc("2006-002 01", "2020-060 02", parseOnly)
	pc("2006-002 02", "2020-060 29", parseOnly)
	pc("2006-002", "2020-000", parseOnly)
	pc("2006-002", "2020-367", parseOnly)
	pc("2006-002", "2020-1", parseOnly)
	for _, v := range []string{
		"2000-01-01T1:12:34Z", "2000-01-01T00:00:00,000Z", "2000-01-01T00:00:00+24:00",
		"2000-01-01T00:00:00+00:60", "2000-01-01T00:00:00+123:45", "2000-02-30T00:00:00Z",
		"2000-13-01T00:00:00Z", "2000-01-01T24:00:00Z", "2000-01-01T00:60:00Z",
		"2000-01-01T00:00:60Z", "2000-01-01T00:00:00.Z", "2000-01-01T00:00:00.1234567891Z",
		"2000-01-01T00:00:00z", "2000-01-01t00:00:00Z", "2000-01-01T00:00:00+0100",
		"2000-01-01T00:00:00-08:00", "1996-12-19T16:39:57-08:00", "2000-07-01T00:00:00-07:00",
		"2000-01-01T00:00:00+05:30", "+000-01-01T00:00:00Z", "2000-01-01T00:00:00Z ",
		"1-01-01T00:00:00Z", "99999-01-01T00:00:00Z", "",
	} {
		pc(RFC3339, v, parseOnly)
		pc(RFC3339Nano, v, parseOnly)
		pc(RFC3339, v, 2)
		pc(RFC3339, v, locUTC)
	}
	for _, v := range []struct{ layout, value string }{
		{"15:04_20060102", "14:38_20150618"}, {"01 MST", "0 MST"}, {"01 MST", "1 MST"},
		{RFC850, "Thursday, 04-Feb-1 21:00:57 PST"}, {RubyDate, "Thu Feb 02 16:10:03 -0500 2006"},
		{RubyDate, "Mon Jan 02 15:04:05 +0123 2006"}, {"3:04PM", "12:00AM"}, {"3:04PM", "12:00PM"},
		{"3:04pm", "12:00am"}, {"3:04pm", "12:00pm"}, {"3:04PM", "13:00PM"}, {"3:04PM", "1:00XM"},
		{"MST", "GMT+3"}, {"MST", "GMT-11"}, {"MST", "GMT+24"}, {"MST", "GMT+"}, {"MST", "ChST"},
		{"MST", "MeST"}, {"MST", "WITA"}, {"MST", "ACDT"}, {"MST", "AEST"}, {"MST", "ABCDT"},
		{"MST", "ABCDE"}, {"MST", "+03"}, {"MST", "-1130"}, {"MST", "UTC"}, {"MST", "PDT"},
		{"MST", "PST"}, {"MST", "EST"}, {"MST 2006-01-02", "PDT 2020-01-15"},
		{"MST 2006-01-02", "PST 2020-07-15"}, {"-0700 MST", "-0800 PST"}, {"-0700 MST", "-0800 PDT"},
		{"-0700 MST", "+0100 CET"}, {"Z07", "Z"}, {"Z07", "+05"}, {"Z0700", "+2500"},
		{"-0700", "+0061"}, {"-07:00:00", "+00:00:61"}, {"-07:00", "+01-00"}, {"-0700", "x0100"},
		{"2006", "-001"}, {"2006", "+001"}, {"06", "+1"}, {"06", "-1"}, {"06", "68"}, {"06", "69"},
		{"15:04:05.000", "12:00:00.-12"}, {"15:04:05.000", "12:00:00.1"}, {"15:04:05", "12:00:00.-1"},
		{"15:04:05 .9", "12:00:00 .x"}, {"15:04:05", "12:00:00,5"}, {"15:04:05.99", "12:00:00.123456"},
		{"Jan", "JAN"}, {"Jan", "jan"}, {"January", "JANUARY"}, {"Mon", "mon"}, {"Monday", "tUESDAY"},
		{"Jan 2", "Feb 31"}, {"2006 Jan 2", "2021 Feb 29"}, {"_2", " 7"}, {"_2", "7"}, {"_2", "  7"},
		{"__2", "  7"}, {"002", "7"}, {"2 Jan", "2  Jan"}, {"2  Jan", "2 Jan"}, {"2 Jan", "2Jan"},
		{"15", "24"}, {"15", "7"}, {"04", "7"}, {"05", "60"}, {"4", "60"}, {"", ""}, {"", "x"},
		{"x", ""}, {"2006\xff", "2020\xff"}, {"2006 \xff", "2020 \xfe"},
	} {
		pc(v.layout, v.value, parseOnly)
		pc(v.layout, v.value, 2)
	}
	w("};\n\n")

	// String, GoString and the text and JSON encodings.
	w("static const SCase g_strings[] = {\n")
	for li := locLocal; li < nloc; li++ {
		loc := locOf(li)
		for _, in := range insts {
			tm := Unix(in.sec, in.nsec).In(loc)
			text, terr := tm.MarshalText()
			js, jerr := tm.MarshalJSON()
			w("    {%d, INT64_C(%d), %d, %s, %s, %s, %s, %s, %s},\n", li, in.sec, in.nsec, cq(tm.String()), cq(tm.GoString()), cq(string(text)), cerr(terr), cq(string(js)), cerr(jerr))
		}
	}
	w("};\n\n")

	w("static const UCase g_unmarshals[] = {\n")
	sentinel := Date(1999, 9, 9, 9, 9, 9, 9, UTC)
	for _, v := range []string{
		`null`, `"null"`, `{}`, `[]`, `"`, `""`, `"x"`, `2000-01-01T00:00:00Z`,
		`"2000-01-01T00:00:00Z`, `2000-01-01T00:00:00Z"`, `"9999-04-12T23:20:50.52Z"`,
		`"1996-12-19T16:39:57-08:00"`, `"0000-01-01T00:00:00.000000001+00:01"`,
		`"2020-01-01T00:00:00+23:59"`, `"2000-01-01T1:12:34Z"`, `"2000-01-01T00:00:00,000Z"`,
		`"2000-01-01T00:00:00+24:00"`, `"2000-01-01T00:00:00+00:60"`,
		`"2000-01-01T00:00:00+123:45"`, `"2000-07-01T00:00:00-07:00"`, `"2000-01-01 00:00:00Z"`,
		`"2000-01-01T00:00:00.123456789123Z"`, `NULL`, `nul`,
	} {
		for kind := 0; kind < 2; kind++ {
			tm := sentinel
			var err error
			if kind == 0 {
				err = tm.UnmarshalText([]byte(v))
			} else {
				err = tm.UnmarshalJSON([]byte(v))
			}
			zn, zo := tm.Zone()
			w("    {%d, %s, %s, %d, INT64_C(%d), %d, %s, %d, %s},\n", kind, cq(v), cerr(err), cbool(tm.Equal(sentinel)), tm.Unix(), tm.Nanosecond(), cq(zn), zo, cq(tm.Location().String()))
		}
	}
	w("};\n\n")

	w("static const ZCase g_tz_names[] = {\n")
	for _, tt := range parseTimeZoneTests {
		n, ok := ParseTimeZone(tt.value)
		w("    {%s, %d, %d},\n", cq(tt.value), n, cbool(ok))
	}
	for _, v := range []string{"", "AB", "ABC", "ABCD", "ABCDT", "ABCDE", "ABCDEF", "ABCT", "Abc", "GMT", "GMT+", "GMT+1", "GMT+24", "GMT-23x", "+", "+2", "+24", "-9", "WITA", "WIT", "ChSTx", "MeS"} {
		n, ok := ParseTimeZone(v)
		w("    {%s, %d, %d},\n", cq(v), n, cbool(ok))
	}
	w("};\n")
	os.Stdout.WriteString(genOut.String())
}
GO

printf '{"Replace":{"%s/zz_gen_test.go":"%s/zz_gen_test.go"}}' "$pkg" "$tmp" > "$tmp/overlay.json"
GEN_ZIP="$goroot/lib/time/zoneinfo.zip" go test -overlay "$tmp/overlay.json" -run '^TestZZGen$' -count=1 -v time > "$tmp/out.txt" ||
    { grep -m1 -B2 -A30 "^panic\|^fatal\|FAIL\|cannot\|undefined\|zz_gen" "$tmp/out.txt" >&2; exit 1; }
sed -n '/^\/\* What Go/,$p' "$tmp/out.txt" | sed '/^--- PASS/,$d' > "$out"
if command -v clang-format >/dev/null 2>&1; then
    clang-format -i "$out"
fi
echo "wrote $out"
