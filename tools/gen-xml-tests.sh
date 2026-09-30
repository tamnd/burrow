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

// What Unmarshal makes of some XML, into a new value or, with keep, into v as
// it is: the error text and a dump of the value. With ns set it decodes with
// that DefaultSpace, as TestUnmarshalNS does.
var unmarshalCases strings.Builder
var unmarshalKept, unmarshalSkipped int

func genUnmarshalCase(name string, v any, in, ns string, keep bool) {
	rv := reflect.ValueOf(v)
	t := rv.Type().Elem()
	if renamed[t] || !supported(t, map[reflect.Type]bool{}) {
		leftOutU[t.String()]++
		unmarshalSkipped++
		return
	}
	_, typ := ctype(t)
	mk := "NULL"
	if keep {
		// builderFor names its function after genKept, the marshal case count.
		kept := genKept
		genKept = 100000 + unmarshalKept
		f, ok := builderFor(rv.Elem())
		genKept = kept
		if !ok {
			leftOutU[t.String()]++
			unmarshalSkipped++
			return
		}
		mk = f
	}
	var err error
	if ns != "" {
		d := NewDecoder(strings.NewReader(in))
		d.DefaultSpace = ns
		err = d.Decode(v)
	} else {
		err = Unmarshal([]byte(in), v)
	}
	var ud strings.Builder
	dump(&ud, rv.Elem(), map[uintptr]bool{})
	fmt.Fprintf(&unmarshalCases, "    {%s, %s, %s, %s, %s, %s, %s},\n",
		lit(name), typ, mk, qs(in), qs(ns), errText(err), lit(ud.String()))
	unmarshalKept++
}

// Types that read_test.go declares inside its test functions.
type ParamVal struct {
	Int int `xml:"int,attr"`
}

type ParamPtr struct {
	Int *int `xml:"int,attr"`
}

type ParamStringPtr struct {
	Int *string `xml:"int,attr"`
}

type IntoNilT struct {
	A int `xml:"A"`
}

// The populated Parent of TestUnmarshalEmptyValues.
func populatedParent() *Parent {
	vBytes0, vInt0, vStr0, vFloat0, vBool0 := []byte("x"), 1, "x", float32(1), true
	vBytes1, vInt1, vStr1, vFloat1, vBool1 := []byte("x"), 1, "x", float32(1), true
	vInt2, vStr2, vFloat2, vBool2 := 1, "x", float32(1), true
	return &Parent{
		I:            vInt0,
		IPtr:         &vInt1,
		Is:           []int{vInt0},
		IPtrs:        []*int{&vInt2},
		F:            vFloat0,
		FPtr:         &vFloat1,
		Fs:           []float32{vFloat0},
		FPtrs:        []*float32{&vFloat2},
		B:            vBool0,
		BPtr:         &vBool1,
		Bs:           []bool{vBool0},
		BPtrs:        []*bool{&vBool2},
		Bytes:        vBytes0,
		BytesPtr:     &vBytes1,
		S:            vStr0,
		SPtr:         &vStr1,
		Ss:           []string{vStr0},
		SPtrs:        []*string{&vStr2},
		MyI:          MyInt(vInt0),
		Child:        Child{G: struct{ I int }{I: vInt0}},
		Children:     []Child{{G: struct{ I int }{I: vInt0}}},
		ChildPtr:     &Child{G: struct{ I int }{I: vInt0}},
		ChildToEmbed: ChildToEmbed{X: vBool0},
	}
}

// The read_test.go tables and one-off values whose types have no methods.
func genReadCases() {
	for i, pt := range pathTests {
		genUnmarshalCase(fmt.Sprintf("pathTests[%d]", i), reflect.New(reflect.TypeOf(pt).Elem()).Interface(), pathTestString, "", false)
	}
	for i, tt := range badPathTests {
		genUnmarshalCase(fmt.Sprintf("badPathTests[%d]", i), tt.v, pathTestString, "", false)
	}
	genUnmarshalCase("TestUnmarshalWithoutNameType", new(TestThree), withoutNameTypeData, "", false)
	for _, v := range []any{new(ParamPtr), new(ParamVal), new(ParamStringPtr)} {
		genUnmarshalCase("TestUnmarshalAttr "+reflect.TypeOf(v).Elem().Name(), v, `<Param int="1" />`, "", false)
	}
	for i, tt := range tables {
		genUnmarshalCase(fmt.Sprintf("tables[%d]", i), new(Tables), tt.xml, tt.ns, false)
	}
	for i, tt := range tableAttrs {
		genUnmarshalCase(fmt.Sprintf("tableAttrs[%d]", i), new(TableAttrs), tt.xml, tt.ns, false)
	}
	for i, s := range []string{
		"<X><!-- a---></X>",
		"<X><!-- -- --></X>",
		"<X><!-- a--b --></X>",
		"<X><!------></X>",
	} {
		genUnmarshalCase(fmt.Sprintf("TestMalformedComment[%d]", i), new(X), s, "", false)
	}
	genUnmarshalCase("TestInvalidInnerXMLType", new(IXField), `<tag><five>5</five><innertag/></tag>`, "", false)
	genUnmarshalCase("TestUnmarshalEmptyValues zero", new(Parent), emptyXML, "", false)
	genUnmarshalCase("TestUnmarshalEmptyValues populated", populatedParent(), emptyXML, "", true)
	genUnmarshalCase("TestUnmarshalWhitespaceValues", new(WhitespaceValuesParent), whitespaceValuesXML, "", false)
	genUnmarshalCase("TestUnmarshalWhitespaceAttrs", new(WhitespaceAttrsParent), whitespaceAttrsXML, "", false)
	genUnmarshalCase("TestUnmarshalIntoInterface empty", new(Pod), `<Pod><Pea><Cotelydon>Green stuff</Cotelydon></Pea></Pod>`, "", false)
}

var leftOutU = map[string]int{}

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
	for _, v := range []any{new(TableAttrs), new(Parent)} {
		markRenames(reflect.TypeOf(v), seen)
	}
	for i, tt := range marshalTests {
		if tt.UnmarshalOnly {
			continue
		}
		genCase(fmt.Sprintf("marshalTests[%d]", i), tt.Value, false, "", "")
	}
	for i, tt := range marshalTests {
		if tt.MarshalOnly {
			continue
		}
		genUnmarshalCase(fmt.Sprintf("marshalTests[%d]", i), reflect.New(reflect.TypeOf(tt.Value).Elem()).Interface(), tt.ExpectXML, "", false)
	}
	genReadCases()
	for i, tt := range marshalErrorTests {
		genCase(fmt.Sprintf("marshalErrorTests[%d]", i), tt.Value, false, "", "")
	}
	for i, tt := range marshalIndentTests {
		genCase(fmt.Sprintf("marshalIndentTests[%d]", i), tt.Value, true, tt.Prefix, tt.Indent)
	}

	var w strings.Builder
	w.WriteString(`/* What Go's encoding/xml writes for the values in its own marshalTests,
 * marshalErrorTests and marshalIndentTests, and what Unmarshal makes of the
 * XML in marshalTests and read_test.go, go1.27.1. Generated by
 * tools/gen-xml-tests.sh; do not edit. */

typedef struct QStr {
    const char *p;
    long long n;
} QStr;

#define QS(x) {x, (long long)sizeof(x) - 1}

typedef struct UCase {
    const char *name;
    const Type *type;
    void (*make)(Alloc *a, void *out);
    QStr in;
    QStr ns;
    const char *err;
    const char *dump;
} UCase;

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
	fmt.Fprintf(&w, "/* What Unmarshal makes of the XML of the marshalTests values and of the\n * read_test.go cases: %d values, %d left out, of these types:\n *\n", unmarshalKept, unmarshalSkipped)
	names = names[:0]
	for n := range leftOutU {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		fmt.Fprintf(&w, " *   %s (%d)\n", n, leftOutU[n])
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
	w.WriteString("};\n\nstatic const UCase u_cases[] = {\n")
	w.WriteString(unmarshalCases.String())
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
