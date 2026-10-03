#!/bin/sh
# Regenerates tests/encoding_jsonv2_test_gen.h: what Go's encoding/json/v2 does
# with the cases in its own TestMarshal and TestUnmarshal. Those tables are Go
# values of Go types, so this does more than record outcomes. It lays a copy of
# arshal_test.go over the package with go test -overlay, with each loop body
# swapped for a call that walks the case's type with reflect and writes the C
# struct and the Type descriptor for it, writes a C function that builds the
# case's value, runs the case, and writes down what Go did. An unmarshalled
# value is written down as a dump both sides know how to make, so the C test
# can compare values without either side knowing the other's layout.
#
# A case is left out when its types have methods, other than jsontext.Value,
# time.Time and time.Duration, since the generator cannot write C for those,
# or when it needs something C has no spelling for here: a complex number, a
# non-nil channel or func, an interface other than any, or a field that would
# look embedded in C without being embedded in Go. The generated header says how many cases went in and
# how many were left out. The reflect walking is shared with
# tools/gen-json-tests.sh and lives in tools/gen-json-common/common.go.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/encoding_jsonv2_test_gen.h"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pkg=$(go env GOROOT)/src/encoding/json/v2

# The table loops of TestMarshal and TestUnmarshal hand each case to the
# recorder and move on.
awk '
/^func TestMarshal\(/ { fn = "m" }
/^func TestUnmarshal\(/ { fn = "u" }
/^func / && !/^func Test(Marshal|Unmarshal)\(/ { fn = "" }
/^\tfor _, tt := range tests \{$/ && fn == "m" {
    print; print "\t\tgenMarshal(tt.name.Name, tt.opts, tt.in, tt.canonicalize, tt.useWriter)"; print "\t\tcontinue"; fn = ""; next
}
/^\tfor _, tt := range tests \{$/ && fn == "u" {
    print; print "\t\tgenUnmarshal(tt.name.Name, tt.opts, tt.inBuf, tt.inVal, tt.skip)"; print "\t\tcontinue"; fn = ""; next
}
{ print }
' "$pkg/arshal_test.go" > "$tmp/arshal_test.go"

cat > "$tmp/zz_gen_test.go" <<'GO'
//go:build goexperiment.jsonv2

package json

import (
	"fmt"
	"os"
	"reflect"
	"strings"
	"testing"
	"time"

	"encoding/json/internal/jsonflags"
	"encoding/json/internal/jsonopts"
	"encoding/json/jsontext"
)

// time.Time and time.Duration have arshalers of their own in v2, which the C
// side has too.
func knownType(t reflect.Type) (string, string, bool) {
	switch t {
	case timeType:
		return "Time", "&burrow_type_Time", true
	case reflect.TypeFor[time.Duration]():
		return "Duration", "&burrow_type_Duration", true
	}
	return "", "", false
}

// A time.Time is built from its instant, then put in UTC or a fixed zone. A
// Duration is an int64, which the common builder already writes.
func knownValue(g *builder, v reflect.Value, lv string) bool {
	if v.Type() != timeType {
		return false
	}
	if !v.CanInterface() {
		g.ok = false
		return true
	}
	tm := v.Interface().(time.Time)
	name, off := tm.Zone()
	fmt.Fprintf(&g.b, "%s = time_from_unix((int64_t)UINT64_C(%d), %d);\n", lv, uint64(tm.Unix()), tm.Nanosecond())
	switch tm.Location() {
	case time.UTC:
		fmt.Fprintf(&g.b, "%s = time_utc(%s);\n", lv, lv)
	case time.Local:
		g.ok = false
	default:
		fmt.Fprintf(&g.b, "%s = time_in(%s, time_fixed_zone(a, (Str)%s, %d));\n", lv, lv, strLit(name), off)
	}
	return true
}

// The C descriptor has the Go name.
func typeName(t reflect.Type) string { return t.Name() }

// The options as the C test spells them, and whether it can.
func optsLit(opts []Options) (string, bool) {
	var s jsonopts.Struct
	s.Join(opts...)
	if s.Flags.Has(jsonflags.Marshalers | jsonflags.Unmarshalers | jsonflags.FormatTag) {
		return "", false
	}
	return fmt.Sprintf("{%#xULL, %#xULL, %s, %s, %d, %d}", s.Flags.Presence, s.Flags.Values,
		qs(s.Indent), qs(s.IndentPrefix), s.ByteLimit, s.DepthLimit), true
}

func genMarshal(name string, opts []Options, in any, canonicalize, useWriter bool) {
	ol, ok := optsLit(opts)
	if !ok {
		genSkipped++
		return
	}
	typ, mk := "NULL", "NULL"
	if in != nil {
		v := reflect.ValueOf(in)
		f, ok := builderFor(v)
		if !ok {
			genSkipped++
			return
		}
		_, typ = ctype(v.Type())
		mk = f
	}
	got, err := Marshal(in, opts...)
	if canonicalize {
		(*jsontext.Value)(&got).Canonicalize()
	}
	c := 0
	if canonicalize {
		c = 1
	}
	fmt.Fprintf(&genCases, "    {%s, 1, %s, %s, %s, QS(\"\"), %s, %s, NULL, %d},\n",
		lit(name), ol, typ, mk, qs(string(got)), errText(err), c)
	genKept++
}

func genUnmarshal(name string, opts []Options, inBuf string, inVal any, skip bool) {
	ol, ok := optsLit(opts)
	if !ok || skip || inVal == nil {
		genSkipped++
		return
	}
	pv := reflect.ValueOf(inVal)
	if pv.Kind() != reflect.Pointer || pv.IsNil() {
		genSkipped++
		return
	}
	f, ok := builderFor(pv.Elem())
	if !ok {
		genSkipped++
		return
	}
	_, typ := ctype(pv.Elem().Type())
	err := Unmarshal([]byte(inBuf), inVal, opts...)
	var d strings.Builder
	dump(&d, pv.Elem(), map[uintptr]bool{})
	fmt.Fprintf(&genCases, "    {%s, 0, %s, %s, %s, %s, QS(\"\"), %s, %s, 0},\n",
		lit(name), ol, typ, f, qs(inBuf), errText(err), lit(d.String()))
	genKept++
}

func TestZZGen(t *testing.T) {
	var w strings.Builder
	w.WriteString(`/* What Go's encoding/json/v2 does with the cases of its own TestMarshal and
 * TestUnmarshal, go1.27.1. Generated by tools/gen-jsonv2-tests.sh; do not edit. */

typedef struct QStr {
    const char *p;
    long long n;
} QStr;

#define QS(x) {x, (long long)sizeof(x) - 1}

typedef struct JvOpts {
    unsigned long long presence, values;
    QStr indent, prefix;
    long long byte_limit, depth_limit;
} JvOpts;

typedef struct JvCase {
    const char *name;
    int marshal;
    JvOpts opts;
    const Type *type;
    void (*mk)(Alloc *a, void *out);
    QStr in;
    QStr out;
    const char *err;
    const char *dump;
    int canonicalize;
} JvCase;

`)
	fmt.Fprintf(&w, "/* %d cases, %d left out. */\n\n", genKept, genSkipped)
	for i := range genTypeOrder {
		fmt.Fprintf(&w, "static Type gt%d;\n", i)
	}
	w.WriteString("\n")
	w.WriteString(genTypeDefs.String())
	w.WriteString("\n")
	w.WriteString(genTypeDescs.String())
	w.WriteString("\nstatic void gen_init(void) {\n")
	w.WriteString(genTypeInit.String())
	w.WriteString("}\n\n")
	w.WriteString(genFuncs.String())
	w.WriteString("static const JvCase jv_cases[] = {\n")
	w.WriteString(genCases.String())
	w.WriteString("};\n")
	os.Stdout.WriteString(w.String())
}
GO

printf '{"Replace":{"%s/arshal_test.go":"%s/arshal_test.go","%s/zz_gen_test.go":"%s/zz_gen_test.go","%s/zz_gen_common_test.go":"%s/tools/gen-json-common/common.go"}}' \
    "$pkg" "$tmp" "$pkg" "$tmp" "$pkg" "$root" > "$tmp/overlay.json"
GOEXPERIMENT=jsonv2 go test -overlay "$tmp/overlay.json" -run '^(TestMarshal|TestUnmarshal|TestZZGen)$' -count=1 -v \
    encoding/json/v2 > "$tmp/out.txt" || { grep -m1 -B2 -A30 "^panic\|^fatal\|stack overflow" "$tmp/out.txt" >&2; exit 1; }
sed -n '/^\/\* What Go/,$p' "$tmp/out.txt" | sed '/^--- PASS/,$d' > "$out"
if command -v clang-format >/dev/null 2>&1; then
    clang-format -i "$out"
fi
echo "wrote $out"
