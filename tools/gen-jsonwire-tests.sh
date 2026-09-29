#!/bin/sh
# Regenerates tests/encoding_jsonwire_test_gen.h: what Go's
# encoding/json/internal/jsonwire does with its own test tables. The tables
# live inside the test functions, so this lifts each one out of Go's source as
# it stands, drops it into a function that runs the same calls, and lays that
# over the package with go test -overlay. What gets printed is what Go did,
# which the C test then has to match. See gen-jsontext-tests.sh for the same
# thing one package up.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/encoding_jsonwire_test_gen.h"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pkg=$(go env GOROOT)/src/encoding/json/internal/jsonwire

# The body of Go's test function up to the end of its table.
table() {
    sed -n "/^func $1(/,/^	}\$/p" "$pkg"/*_test.go | sed 1d
}

{
    cat <<'GO'
//go:build goexperiment.jsonv2

package jsonwire

import (
	"errors"
	"fmt"
	"io"
	"math"
	"math/rand"
	"os"
	"strings"
	"testing"

	"encoding/json/internal/jsonflags"
)

var (
	_ = errors.New
	_ = io.EOF
	_ = strings.Repeat
	_ jsonflags.Bools
)

var w strings.Builder

func pf(format string, args ...any) { fmt.Fprintf(&w, format, args...) }

// C string literal of s, with octal escapes so nothing runs into the next
// character.
func lit(s string) string {
	var b strings.Builder
	b.WriteByte('"')
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == '"' || c == '\\':
			b.WriteByte('\\')
			b.WriteByte(c)
		case c == '?':
			b.WriteString(`\077`)
		case c >= ' ' && c <= '~':
			b.WriteByte(c)
		default:
			fmt.Fprintf(&b, "\\%03o", c)
		}
	}
	b.WriteByte('"')
	return b.String()
}

// Strings too long for a C literal come out as a prefix and a unit repeated
// some number of times, which the C side puts back together.
func qs(s string) string {
	if len(s) > 1000 {
		for pre := 0; pre < 4; pre++ {
			for n := 1; n <= 16; n++ {
				unit := s[pre : pre+n]
				if k := (len(s) - pre) / n; s[pre:] == strings.Repeat(unit, k) {
					return fmt.Sprintf("QR(%s, %s, %d)", lit(s[:pre]), lit(unit), k)
				}
			}
		}
		panic("no repeat in long string")
	}
	return "QS(" + lit(s) + ")"
}

func errText(err error) string {
	if err == nil {
		return ""
	}
	return err.Error()
}

func b2i(b bool) int {
	if b {
		return 1
	}
	return 0
}

func genConsumeWhitespace() {
GO
    table TestConsumeWhitespace
    cat <<'GO'
	pf("static const JwWhitespaceCase jw_whitespace_cases[] = {\n")
	for _, tt := range tests {
		pf("    {%s, %d},\n", qs(tt.in), ConsumeWhitespace([]byte(tt.in)))
	}
	pf("};\n\n")
}

func genConsumeLiteral() {
GO
    table TestConsumeLiteral
    cat <<'GO'
	pf("static const JwLiteralCase jw_literal_cases[] = {\n")
	for _, tt := range tests {
		var simple int
		switch tt.literal {
		case "null":
			simple = ConsumeNull([]byte(tt.in))
		case "false":
			simple = ConsumeFalse([]byte(tt.in))
		case "true":
			simple = ConsumeTrue([]byte(tt.in))
		}
		got, err := ConsumeLiteral([]byte(tt.in), tt.literal)
		pf("    {%s, %s, %d, %d, %s},\n", qs(tt.literal), qs(tt.in), simple, got, lit(errText(err)))
	}
	pf("};\n\n")
}

func genConsumeString() {
GO
    table TestConsumeString
    cat <<'GO'
	_ = errPrev
	pf("static const JwStringCase jw_string_cases[] = {\n")
	for _, tt := range tests {
		simple := ConsumeSimpleString([]byte(tt.in))
		var flags ValueFlags
		got, err := ConsumeString(&flags, []byte(tt.in), false)
		var flags8 ValueFlags
		got8, err8 := ConsumeString(&flags8, []byte(tt.in), true)
		unq, errq := AppendUnquote(nil, []byte(tt.in))
		pf("    {%s, %d, %d, %d, %s, %d, %d, %s, %s, %s},\n", qs(tt.in), simple, flags, got,
			lit(errText(err)), flags8, got8, lit(errText(err8)), qs(string(unq)), lit(errText(errq)))
	}
	pf("};\n\n")
}

func genConsumeNumber() {
GO
    table TestConsumeNumber
    cat <<'GO'
	pf("static const JwNumberCase jw_number_cases[] = {\n")
	for _, tt := range tests {
		got, err := ConsumeNumber([]byte(tt.in))
		pf("    {%s, %d, %d, %s},\n", qs(tt.in), ConsumeSimpleNumber([]byte(tt.in)), got,
			lit(errText(err)))
	}
	pf("};\n\n")
}

func genParseHexUint16() {
GO
    table TestParseHexUint16
    cat <<'GO'
	pf("static const JwParseCase jw_hex_cases[] = {\n")
	for _, tt := range tests {
		got, ok := parseHexUint16([]byte(tt.in))
		pf("    {%s, %#xULL, %d},\n", qs(tt.in), got, b2i(ok))
	}
	pf("};\n\n")
}

func genParseUint() {
GO
    table TestParseUint
    cat <<'GO'
	pf("static const JwParseCase jw_uint_cases[] = {\n")
	for _, tt := range tests {
		got, ok := ParseUint([]byte(tt.in))
		pf("    {%s, %#xULL, %d},\n", qs(tt.in), got, b2i(ok))
	}
	pf("};\n\n")
}

func genAppendQuote() {
GO
    table TestAppendQuote
    cat <<'GO'
	pf("static const JwQuoteCase jw_quote_cases[] = {\n")
	for _, tt := range tests {
		var flags jsonflags.Flags
		flags.Set(tt.flags | 1)
		flags.Set(jsonflags.AllowInvalidUTF8 | 1)
		got, err := AppendQuote(nil, []byte(tt.in), &flags)
		flags.Set(jsonflags.AllowInvalidUTF8 | 0)
		got8, err8 := AppendQuote(nil, []byte(tt.in), &flags)
		pf("    {%s, %#xULL, %s, %s, %s, %s},\n", qs(tt.in), uint64(tt.flags), qs(string(got)),
			lit(errText(err)), qs(string(got8)), lit(errText(err8)))
	}
	pf("};\n\n")
}

func genAppendNumber() {
GO
    table TestAppendNumber
    cat <<'GO'
	pf("static const JwFloatCase jw_float_cases[] = {\n")
	for _, tt := range tests {
		pf("    {%#xULL, %s, %s},\n", math.Float64bits(tt.in), qs(string(AppendFloat(nil, tt.in, 32))),
			qs(string(AppendFloat(nil, tt.in, 64))))
	}
	pf("};\n\n")
}

func genQuoteRune() {
GO
    table TestQuoteRune
    cat <<'GO'
	pf("static const JwPairCase jw_quote_rune_cases[] = {\n")
	for _, tt := range tests {
		pf("    {%s, %s},\n", qs(tt.in), qs(QuoteRune([]byte(tt.in))))
	}
	pf("};\n\n")
}

func genTruncatePointer() {
GO
    table TestTruncatePointer
    cat <<'GO'
	pf("static const JwPairCase jw_truncate_cases[] = {\n")
	for _, tt := range tests {
		pf("    {%s, %s},\n", qs(tt.in), qs(TruncatePointer(tt.in, 10)))
	}
	pf("};\n\n")
}

// Go's own compareUTF16Testdata, in order, and then random pairs of short
// strings, valid and not, with what CompareUTF16 said about each.
func genCompareUTF16() {
	pf("static const QStr jw_compare_data[] = {\n")
	for _, s := range compareUTF16Testdata {
		pf("    %s,\n", qs(s))
	}
	pf("};\n\n")
	alphabet := []string{"", "a", "z", "\x7f", "\u0080", "ö", "߿", "ࠀ", "€",
		"퟿", "", "דּ", "￿", "\U00010000", "\U0001f600", "\U0010ffff", "\xfe",
		"\xff", "\xc0", "\xed\xa0\x80"}
	r := rand.New(rand.NewSource(1))
	word := func() string {
		var b strings.Builder
		for n := r.Intn(4); n > 0; n-- {
			b.WriteString(alphabet[r.Intn(len(alphabet))])
		}
		return b.String()
	}
	pf("static const JwCompareCase jw_compare_cases[] = {\n")
	for range 400 {
		x, y := word(), word()
		pf("    {%s, %s, %d},\n", qs(x), qs(y), CompareUTF16([]byte(x), []byte(y)))
	}
	pf("};\n\n")
}

func TestZZGen(t *testing.T) {
	pf(`/* What Go's encoding/json/internal/jsonwire does with its own test tables,
 * go1.27.1. Generated by tools/gen-jsonwire-tests.sh; do not edit. */

/* A string, or when unit is set, p followed by unit reps times. */
typedef struct QStr {
    const char *p;
    long long n;
    const char *unit;
    long long reps;
} QStr;

#define QS(x) {x, (long long)sizeof(x) - 1, NULL, 0}
#define QR(x, u, k) {x, (long long)sizeof(x) - 1, u, k}

typedef struct JwWhitespaceCase {
    QStr in;
    long long got;
} JwWhitespaceCase;

typedef struct JwLiteralCase {
    QStr literal, in;
    long long simple, got;
    const char *err;
} JwLiteralCase;

/* ConsumeSimpleString, ConsumeString without and with UTF-8 checks, and
 * AppendUnquote. */
typedef struct JwStringCase {
    QStr in;
    long long simple;
    unsigned flags;
    long long got;
    const char *err;
    unsigned flags8;
    long long got8;
    const char *err8;
    QStr unquoted;
    const char *unquote_err;
} JwStringCase;

typedef struct JwNumberCase {
    QStr in;
    long long simple, got;
    const char *err;
} JwNumberCase;

typedef struct JwParseCase {
    QStr in;
    unsigned long long got;
    int ok;
} JwParseCase;

/* AppendQuote with invalid UTF-8 allowed and then not. */
typedef struct JwQuoteCase {
    QStr in;
    unsigned long long flags;
    QStr got;
    const char *err;
    QStr got8;
    const char *err8;
} JwQuoteCase;

typedef struct JwFloatCase {
    unsigned long long bits;
    QStr got32, got64;
} JwFloatCase;

typedef struct JwPairCase {
    QStr in, got;
} JwPairCase;

typedef struct JwCompareCase {
    QStr x, y;
    int got;
} JwCompareCase;

`)
	genConsumeWhitespace()
	genConsumeLiteral()
	genConsumeString()
	genConsumeNumber()
	genParseHexUint16()
	genParseUint()
	genAppendQuote()
	genAppendNumber()
	genQuoteRune()
	genTruncatePointer()
	genCompareUTF16()
	os.Stdout.WriteString(w.String())
}
GO
} > "$tmp/zz_gen_test.go"

printf '{"Replace":{"%s/zz_gen_test.go":"%s/zz_gen_test.go"}}' "$pkg" "$tmp" > "$tmp/overlay.json"
GOEXPERIMENT=jsonv2 go test -overlay "$tmp/overlay.json" -run '^TestZZGen$' -count=1 -v \
    encoding/json/internal/jsonwire > "$tmp/out.txt" || { cat "$tmp/out.txt" >&2; exit 1; }
sed -e '/^=== RUN/d' -e '/^--- PASS/,$d' "$tmp/out.txt" > "$out"

# TestCanonicalNumber starts from a fixed list of float64 bits before it moves
# on to its serial and random numbers. The list is local to the test, so it is
# copied out of the source.
{
    echo '/* The fixed numbers TestCanonicalNumber starts with. */'
    echo 'static const unsigned long long jw_canonical_static[] = {'
    sed -n '/static := \[\.\.\.\]uint64{/,/^		}/p' "$pkg/encode_test.go" | sed -e 1d -e '$d' |
        tr -d '\t' | tr ',' '\n' | sed -e 's/ //g' -e '/^$/d' -e 's/.*/    &ULL,/'
    echo '};'
} >> "$out"
if command -v clang-format >/dev/null 2>&1; then
    clang-format -i "$out"
fi
echo "wrote $out"
