#!/bin/sh
# Regenerates tests/encoding_xml_test_gen.h: what Go's encoding/xml writes for
# the values in its own marshalTests, marshalErrorTests and marshalIndentTests
# tables. It adds a file to the package with go test -overlay that runs each
# value through Marshal or MarshalIndent and writes the C type for it, a
# function that builds the same value, and Go's output or error text.
#
# A value is left out when its type, or a type inside it, has methods, since
# the C side would need the methods too, or when C has no spelling for it. The
# reflect walking is the json generators', in tools/gen-json-common/common.go,
# with Name and Attr mapped onto XmlName and XmlAttr. The generated header says
# how many values went in and which types kept the rest out.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/encoding_xml_test_gen.h"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pkg=$(go env GOROOT)/src/encoding/xml

sed -e '/^\/\/go:build/d' -e 's/^package json$/package xml/' \
    "$root/tools/gen-json-common/common.go" > "$tmp/zz_gen_common_test.go"

cat > "$tmp/zz_gen_test.go" <<'GO'
package xml

import (
	"fmt"
	"os"
	"reflect"
	"sort"
	"strings"
	"testing"
)

var (
	anyType = reflect.TypeFor[any]()
	// Only the json generators have a jsontext.Value. This is a type no test
	// value uses, so that the common code's checks for it never match.
	jsontextValueType = reflect.TypeFor[struct{ genNever_ int }]()
	nameType_         = reflect.TypeFor[Name]()
	attrType_         = reflect.TypeFor[Attr]()
)

func knownType(t reflect.Type) (string, string, bool) {
	switch t {
	case nameType_:
		return "XmlName", "&burrow_type_XmlName", true
	case attrType_:
		return "XmlAttr", "&burrow_type_XmlAttr", true
	}
	return "", "", false
}

func strStore(g *builder, lv, s string) {
	if s != "" {
		fmt.Fprintf(&g.b, "%s = (Str){(const Byte *)%s, %d};\n", lv, lit(s), len(s))
	}
}

func knownValue(g *builder, v reflect.Value, lv string) bool {
	switch v.Type() {
	case nameType_:
		n := v.Interface().(Name)
		strStore(g, lv+".space", n.Space)
		strStore(g, lv+".local", n.Local)
		return true
	case attrType_:
		a := v.Interface().(Attr)
		strStore(g, lv+".name.space", a.Name.Space)
		strStore(g, lv+".name.local", a.Name.Local)
		strStore(g, lv+".value", a.Value)
		return true
	}
	return false
}

// Go can have a field named after its type without embedding it, T1 T1,
// which the C side reads as embedded. Such a type gets an underscore after
// its name on the C side. Only a value marshaled at the top, where the type
// names the element, would show it, and genCase leaves those out.
var renamed = map[reflect.Type]bool{}

func typeName(t reflect.Type) string {
	if renamed[t] {
		return t.Name() + "_"
	}
	return t.Name()
}

func markRenames(t reflect.Type, seen map[reflect.Type]bool) {
	if t == nil || seen[t] {
		return
	}
	seen[t] = true
	switch t.Kind() {
	case reflect.Slice, reflect.Array, reflect.Pointer, reflect.Chan:
		markRenames(t.Elem(), seen)
	case reflect.Map:
		markRenames(t.Key(), seen)
		markRenames(t.Elem(), seen)
	case reflect.Struct:
		for i := 0; i < t.NumField(); i++ {
			f := t.Field(i)
			if !f.Anonymous && f.Type.Name() == f.Name {
				renamed[f.Type] = true
			}
			markRenames(f.Type, seen)
		}
	}
}

// How many values each left-out type accounts for.
var leftOut = map[string]int{}

func genCase(name string, v any, indent bool, prefix, ind string) {
	var data []byte
	var err error
	if indent {
		data, err = MarshalIndent(v, prefix, ind)
	} else {
		data, err = Marshal(v)
	}
	ind0 := "NULL"
	if indent {
		ind0 = qs(prefix) + ", " + qs(ind)
	} else {
		ind0 = "QS(\"\"), QS(\"\")"
	}
	if v == nil {
		fmt.Fprintf(&genCases, "    {%s, NULL, NULL, %d, %s, %s, %s},\n",
			lit(name), b01(indent), ind0, qs(string(data)), errText(err))
		genKept++
		return
	}
	rv := reflect.ValueOf(v)
	top := rv.Type()
	for top.Kind() == reflect.Pointer {
		top = top.Elem()
	}
	f, ok := "", false
	if !renamed[top] {
		f, ok = builderFor(rv)
	}
	if !ok {
		leftOut[rv.Type().String()]++
		genSkipped++
		return
	}
	_, typ := ctype(rv.Type())
	fmt.Fprintf(&genCases, "    {%s, %s, %s, %d, %s, %s, %s},\n",
		lit(name), typ, f, b01(indent), ind0, qs(string(data)), errText(err))
	genKept++
}

func b01(b bool) int {
	if b {
		return 1
	}
	return 0
}

func TestZZGen(t *testing.T) {
	seen := map[reflect.Type]bool{}
	for _, tt := range marshalTests {
		markRenames(reflect.TypeOf(tt.Value), seen)
	}
	for _, tt := range marshalErrorTests {
		markRenames(reflect.TypeOf(tt.Value), seen)
	}
	for _, tt := range marshalIndentTests {
		markRenames(reflect.TypeOf(tt.Value), seen)
	}
	for i, tt := range marshalTests {
		if tt.UnmarshalOnly {
			continue
		}
		genCase(fmt.Sprintf("marshalTests[%d]", i), tt.Value, false, "", "")
	}
	for i, tt := range marshalErrorTests {
		genCase(fmt.Sprintf("marshalErrorTests[%d]", i), tt.Value, false, "", "")
	}
	for i, tt := range marshalIndentTests {
		genCase(fmt.Sprintf("marshalIndentTests[%d]", i), tt.Value, true, tt.Prefix, tt.Indent)
	}

	var w strings.Builder
	w.WriteString(`/* What Go's encoding/xml writes for the values in its own marshalTests,
 * marshalErrorTests and marshalIndentTests, go1.27.1. Generated by
 * tools/gen-xml-tests.sh; do not edit. */

typedef struct QStr {
    const char *p;
    long long n;
} QStr;

#define QS(x) {x, (long long)sizeof(x) - 1}

typedef struct XCase {
    const char *name;
    const Type *type;
    void (*make)(Alloc *a, void *out);
    int indent;
    QStr prefix;
    QStr indent_str;
    QStr out;
    const char *err;
} XCase;

`)
	fmt.Fprintf(&w, "/* %d values, %d left out. The left-out values have these types, which\n * have methods or hold a type that does:\n *\n", genKept, genSkipped)
	names := make([]string, 0, len(leftOut))
	for n := range leftOut {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		fmt.Fprintf(&w, " *   %s (%d)\n", n, leftOut[n])
	}
	w.WriteString(" */\n\n")
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
	w.WriteString("static const XCase x_cases[] = {\n")
	w.WriteString(genCases.String())
	w.WriteString("};\n")
	os.Stdout.WriteString(w.String())
}
GO

printf '{"Replace":{"%s/zz_gen_test.go":"%s/zz_gen_test.go","%s/zz_gen_common_test.go":"%s/zz_gen_common_test.go"}}' \
    "$pkg" "$tmp" "$pkg" "$tmp" > "$tmp/overlay.json"
go test -overlay "$tmp/overlay.json" -run '^TestZZGen$' -count=1 -v \
    encoding/xml > "$tmp/out.txt" || { grep -m1 -B2 -A30 "^panic\|^fatal\|stack overflow\|cannot\|undefined\|FAIL" "$tmp/out.txt" >&2; exit 1; }
sed -n '/^\/\* What Go/,$p' "$tmp/out.txt" | sed '/^--- PASS/,$d' > "$out"
if command -v clang-format >/dev/null 2>&1; then
    clang-format -i "$out"
fi
echo "wrote $out"
