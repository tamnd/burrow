// Writes tests/scan_gen.h: every table in scan_test.go run through Go's own
// scanning, with the count, the error and the value each target ends up
// holding. tools/gen-scan.sh copies this next to scan_test.go and runs it.

package fmt_test

import (
	"bytes"
	. "fmt"
	"os"
	"reflect"
	"strings"
	"testing"
)

const (
	modeScan   = "SM_SCAN"
	modeScanln = "SM_SCANLN"
	modeScanf  = "SM_SCANF"
)

// Every target of every case, in order, so that a case is a start and a count
// into it.
var targets []string

// A C initialiser for a Str holding exactly s, with octal escapes so that a
// digit after one cannot be read as part of it.
func cstr(s string) string {
	var b strings.Builder
	b.WriteString("BURROW_S_INIT(\"")
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == '"' || c == '\\':
			b.WriteByte('\\')
			b.WriteByte(c)
		case c == '?':
			// Trigraphs.
			b.WriteString("\\?")
		case c >= 0x20 && c < 0x7f:
			b.WriteByte(c)
		default:
			Fprintf(&b, "\\%03o", c)
		}
	}
	b.WriteString("\")")
	return b.String()
}

// The Go type a renamed one is spelled as, which is what the C side scans into.
var base = map[reflect.Kind]reflect.Type{
	reflect.Bool:       reflect.TypeFor[bool](),
	reflect.Int:        reflect.TypeFor[int](),
	reflect.Int8:       reflect.TypeFor[int8](),
	reflect.Int16:      reflect.TypeFor[int16](),
	reflect.Int32:      reflect.TypeFor[int32](),
	reflect.Int64:      reflect.TypeFor[int64](),
	reflect.Uint:       reflect.TypeFor[uint](),
	reflect.Uint8:      reflect.TypeFor[uint8](),
	reflect.Uint16:     reflect.TypeFor[uint16](),
	reflect.Uint32:     reflect.TypeFor[uint32](),
	reflect.Uint64:     reflect.TypeFor[uint64](),
	reflect.Uintptr:    reflect.TypeFor[uintptr](),
	reflect.Float32:    reflect.TypeFor[float32](),
	reflect.Float64:    reflect.TypeFor[float64](),
	reflect.Complex64:  reflect.TypeFor[complex64](),
	reflect.Complex128: reflect.TypeFor[complex128](),
	reflect.String:     reflect.TypeFor[string](),
	reflect.Slice:      reflect.TypeFor[[]byte](),
}

// A fresh zero target of the same type as p, and its C type code.
func fresh(p any) (any, string) {
	et := reflect.TypeOf(p).Elem()
	v := reflect.New(et).Interface()
	switch v.(type) {
	case *Xs:
		return v, "SC_XS, false"
	case *IntString:
		return v, "SC_INTSTRING, false"
	}
	name := "SC_" + strings.ToUpper(et.Kind().String())
	if et.Kind() == reflect.Slice {
		name = "SC_BYTES"
	}
	return v, Sprintf("%s, %t", name, et.PkgPath() != "")
}

// What a target holds after the scan, the way the C test prints it.
func show(p any) string {
	switch x := p.(type) {
	case *Xs:
		return Sprintf("%q", string(*x))
	case *IntString:
		return Sprintf("{%d %q}", x.i, x.s)
	}
	v := reflect.ValueOf(p).Elem()
	return Sprintf("%#v", v.Convert(base[v.Kind()]).Interface())
}

func emit(w *bytes.Buffer, mode, format, text string, in []any) {
	var ts []any
	var codes []string
	for _, p := range in {
		t, code := fresh(p)
		ts = append(ts, t)
		codes = append(codes, code)
	}
	var n int
	var err error
	switch mode {
	case modeScan:
		n, err = Sscan(text, ts...)
	case modeScanln:
		n, err = Sscanln(text, ts...)
	default:
		n, err = Sscanf(text, format, ts...)
	}
	e := ""
	if err != nil {
		e = err.Error()
	}
	Fprintf(w, "    {%s, %s, %s, %d, %d, %d, %s},\n", mode, cstr(format), cstr(text),
		len(targets), len(ts), n, cstr(e))
	for i, t := range ts {
		targets = append(targets, Sprintf("{%s, %s}", codes[i], cstr(show(t))))
	}
}

func TestGenScan(t *testing.T) {
	var w bytes.Buffer
	w.WriteString(`/* What Go's fmt does with the cases in scan_test.go: how many operands each
 * scan fills, the error it returns and what every operand holds afterwards.
 * Regenerate it with tools/gen-scan.sh rather than editing it.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

static const ScanCase scan_cases[] = {
`)
	var a, b int
	var s1, s2 string
	var bs1, bs2 []byte
	for _, tt := range scanTests {
		emit(&w, modeScan, "", tt.text, []any{tt.in})
		emit(&w, modeScanln, "", tt.text, []any{tt.in})
	}
	for _, tt := range overflowTests {
		emit(&w, modeScan, "", tt.text, []any{tt.in})
	}
	for _, tt := range scanfTests {
		emit(&w, modeScanf, tt.format, tt.text, []any{tt.in})
	}
	for _, tt := range multiTests {
		emit(&w, modeScanf, tt.format, tt.text, tt.in)
	}
	for _, tt := range eofTests {
		emit(&w, modeScanf, tt.format, "", []any{tt.v})
		emit(&w, modeScanf, tt.format, "   ", []any{tt.v})
	}
	// The tables and one-off cases the rest of scan_test.go writes inline.
	emit(&w, modeScan, "", "123abc", []any{&a, &s1})
	emit(&w, modeScan, "", "asdf", []any{&s1, &a})
	emit(&w, modeScan, "", "abc", []any{&s1, &s2})
	emit(&w, modeScan, "", "", []any{&s1, &s2})
	emit(&w, modeScanf, "%q", `""`, []any{&s1})
	emit(&w, modeScanln, "", "1 x\n", []any{&a})
	emit(&w, modeScanln, "", "123\n456\n", []any{&a, &b})
	emit(&w, modeScanln, "", "123\n", []any{&a})
	emit(&w, modeScanln, "", "", []any{&a})
	emit(&w, modeScanf, "%d %d", "23", []any{&a, &b})
	emit(&w, modeScan, "", "234", []any{&a, &b})
	emit(&w, modeScan, "", "234 ", []any{&a, &b})
	emit(&w, modeScanf, "%x", "00010203", []any{&bs1})
	emit(&w, modeScanf, "%x %x", "00010203 00010203", []any{&bs1, &bs2})
	emit(&w, modeScanf, "%x", "00010203:", []any{&bs1})
	emit(&w, modeScanf, "%x:%x", "00010203:00010203", []any{&bs1, &bs2})
	emit(&w, modeScanf, "%x", "000102034:", []any{&bs1})
	for _, text := range []string{"1\n2\n", "1\n2", "1  \n  2  \n", "1  \n  2"} {
		emit(&w, modeScan, "", text, []any{&a, &b})
	}
	for _, text := range []string{"1\n", "   1 2    \n", "   1 2", "1\n2\n"} {
		emit(&w, modeScanln, "", text, []any{&a, &b})
	}
	for _, tt := range [][2]string{
		{"1\n2", "%d\n%d\n"}, {"1\n2", "%d %d"}, {"1 \n2", "%d %d"}, {"1 2", "%d\n%d"},
		{"1 2", "%d \n%d"}, {"1 \n2", "%d \n%d"}, {"1\n2", "%d\n %d"}, {"1\n2", "%d \n %d"},
		{"1\n2", "%d\n%d"}, {"1\n2", "%d\n %d"}, {"1\n2", "%d \n%d"}, {"1\n2", "%d \n %d"},
		{"1\n 2", "%d\n%d"}, {"1\n 2", "%d\n%d "}, {"1\n 2", "%d \n%d"}, {"1\n 2", "%d \n %d"},
		{"1 \n2", "%d\n%d"}, {"1 \n2", "%d\n %d"}, {"1 \n2", "%d \n%d"}, {"1 \n2", "%d \n %d"},
		{"1 \n 2", "%d\n%d"}, {"1 \n 2", "%d\n %d"}, {"1 \n 2", "%d \n%d"}, {"1 \n 2", "%d \n %d"},
		{"1\n2", "1\n2"}, {"1\n2", "1\n 2"}, {"1\n2", "1 \n2"}, {"1\n2", "1 \n 2"},
		{"1\n 2", "1\n2"}, {"1\n 2", "1\n2 "}, {"1\n 2", "1 \n2"}, {"1\n 2", "1 \n 2"},
		{"1 \n2", "1\n2"}, {"1 \n2", "1\n 2"}, {"1 \n2", "1 \n2"}, {"1 \n2", "1 \n 2"},
		{"1 \n 2", "1\n2"}, {"1 \n 2", "1\n 2"}, {"1 \n 2", "1 \n2"}, {"1 \n 2", "1 \n 2"},
	} {
		if strings.Contains(tt[1], "%") {
			emit(&w, modeScanf, tt[1], tt[0], []any{&a, &b})
		} else {
			emit(&w, modeScanf, tt[1], tt[0], nil)
		}
	}
	n := strings.Count(w.String(), "\n    {")
	w.WriteString("};\n\nstatic const ScanTarget scan_targets[] = {\n")
	for _, t := range targets {
		Fprintf(&w, "    %s,\n", t)
	}
	w.WriteString("};\n")
	if err := os.WriteFile(os.Getenv("OUT"), w.Bytes(), 0o644); err != nil {
		t.Fatal(err)
	}
	t.Logf("%d cases", n)
}
