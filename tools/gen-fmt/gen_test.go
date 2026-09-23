// Writes tests/fmt_gen.h: the cases from fmt_test.go whose operands are values
// burrow can hand to fmt without a type of the test's own, with what Go prints
// for each. tools/gen-fmt.sh copies this next to fmt_test.go and runs it.

package fmt_test

import (
	"bytes"
	. "fmt"
	"math"
	"os"
	"reflect"
	"strings"
	"testing"
)

// The C for one operand, or false when it is not a value the table can carry.
func operand(v any) (string, bool) {
	if v == nil {
		return "{FC_NIL, 0, 0, {NULL, 0}}", true
	}
	t := reflect.TypeOf(v)
	if t.PkgPath() != "" {
		return "", false
	}
	rv := reflect.ValueOf(v)
	switch t.Kind() {
	case reflect.Bool:
		b := uint64(0)
		if rv.Bool() {
			b = 1
		}
		return Sprintf("{FC_BOOL, %d, 0, {NULL, 0}}", b), true
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		return Sprintf("{FC_%s, UINT64_C(%#x), 0, {NULL, 0}}", strings.ToUpper(t.Name()), uint64(rv.Int())), true
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr:
		return Sprintf("{FC_%s, UINT64_C(%#x), 0, {NULL, 0}}", strings.ToUpper(t.Name()), rv.Uint()), true
	case reflect.Float32:
		return Sprintf("{FC_FLOAT32, UINT64_C(%#x), 0, {NULL, 0}}", math.Float32bits(float32(rv.Float()))), true
	case reflect.Float64:
		return Sprintf("{FC_FLOAT64, UINT64_C(%#x), 0, {NULL, 0}}", math.Float64bits(rv.Float())), true
	case reflect.Complex64:
		c := rv.Complex()
		return Sprintf("{FC_COMPLEX64, UINT64_C(%#x), UINT64_C(%#x), {NULL, 0}}",
			math.Float32bits(float32(real(c))), math.Float32bits(float32(imag(c)))), true
	case reflect.Complex128:
		c := rv.Complex()
		return Sprintf("{FC_COMPLEX128, UINT64_C(%#x), UINT64_C(%#x), {NULL, 0}}",
			math.Float64bits(real(c)), math.Float64bits(imag(c))), true
	case reflect.String:
		return Sprintf("{FC_STRING, 0, 0, %s}", cstr(rv.String())), true
	case reflect.Slice:
		if t.Elem().Kind() == reflect.Uint8 && t.Elem().PkgPath() == "" {
			if rv.IsNil() {
				return "{FC_BYTES_NIL, 0, 0, {NULL, 0}}", true
			}
			return Sprintf("{FC_BYTES, 0, 0, %s}", cstr(string(rv.Bytes()))), true
		}
	}
	return "", false
}

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

// Whether Go's own output for this depends on the word size, which only int,
// uint and uintptr can, and only when the value would not fit in 32 bits.
func wide(vals []any) bool {
	for _, v := range vals {
		switch x := v.(type) {
		case int:
			if x != int(int32(x)) {
				return true
			}
		case uint:
			if x != uint(uint32(x)) {
				return true
			}
		case uintptr:
			if x != uintptr(uint32(x)) {
				return true
			}
		}
	}
	return false
}

// Every operand of every case, in order, so that a case is a start and a count
// into it.
var operands []string

func emit(w *bytes.Buffer, format string, vals []any) bool {
	var ops []string
	for _, v := range vals {
		op, ok := operand(v)
		if !ok {
			return false
		}
		ops = append(ops, op)
	}
	out := Sprintf(format, vals...)
	Fprintf(w, "    {%s, %s, %d, %d, %t},\n", cstr(format), cstr(out), len(operands), len(vals),
		wide(vals))
	operands = append(operands, ops...)
	return true
}

func TestGenFmt(t *testing.T) {
	var w bytes.Buffer
	w.WriteString(`/* What Go's fmt prints for the cases in fmt_test.go whose operands are plain
 * values, as opposed to values of a type the test declares. Regenerate it with
 * tools/gen-fmt.sh rather than editing it.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

static const FmtCase fmt_cases[] = {
`)
	n := 0
	for _, tt := range fmtTests {
		if strings.Contains(tt.out, "PTR") {
			continue
		}
		if emit(&w, tt.fmt, []any{tt.val}) {
			n++
		}
	}
	for _, tt := range reorderTests {
		if emit(&w, tt.fmt, tt.val) {
			n++
		}
	}
	for _, tt := range startests {
		if emit(&w, tt.fmt, tt.in) {
			n++
		}
	}
	w.WriteString("};\n\nstatic const FmtOperand fmt_operands[] = {\n")
	for _, op := range operands {
		Fprintf(&w, "    %s,\n", op)
	}
	w.WriteString("};\n")
	if err := os.WriteFile(os.Getenv("OUT"), w.Bytes(), 0o644); err != nil {
		t.Fatal(err)
	}
	t.Logf("%d cases", n)
}
