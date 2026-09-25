// The generator behind tests/cmplx_test_gen.h. tools/gen-cmplx-tests.sh copies
// it next to Go's math/cmplx package and the tables from cmath_test.go and
// runs it.
//
// It writes the tables at the top of cmath_test.go as bit patterns, and one
// hash for each function of what Go returns over a long run of inputs drawn
// the same way tests/cmplx_test.c draws them, as tools/gen-math-tests does for
// math.

package cmplx

import (
	"fmt"
	"math"
	"os"
	"reflect"
	"strings"
	"testing"
)

type table struct {
	name string
	v    any
}

// diffN is the number of inputs each function gets. The C side has the same.
const diffN = 1 << 14

type draws struct{ s uint64 }

// next is xorshift64*, the same as in the math generator.
func (d *draws) next() uint64 {
	d.s ^= d.s >> 12
	d.s ^= d.s << 25
	d.s ^= d.s >> 27
	return d.s * 0x2545F4914F6CDD1D
}

// float draws a value of one of five kinds, each one exact in doubles.
func (d *draws) float(kind int) float64 {
	r := d.next()
	switch kind {
	case 0: // any bits at all, NaNs and infinities included
		return math.Float64frombits(r)
	case 1: // [-1, 1)
		return float64(int64(r>>11)-(1<<52)) * 0x1p-52
	case 2: // [-32, 32)
		return float64(int64(r>>11)-(1<<52)) * 0x1p-47
	case 3: // a binary exponent in [-32, 31]
		return math.Float64frombits(r&0x800fffffffffffff | uint64(1023+int64(r>>52&63)-32)<<52)
	default: // halves in [-16, 16), so zeros and small integers turn up often
		return float64(int64(r>>58)-32) * 0.5
	}
}

type hash struct{ h uint64 }

// add folds a result into an FNV-1a hash. Every NaN counts as the same NaN,
// since the bits of one made by arithmetic depend on the machine.
func (h *hash) add(b uint64) {
	if b&0x7ff0000000000000 == 0x7ff0000000000000 && b&0x000fffffffffffff != 0 {
		b = 0x7ff8000000000000
	}
	for i := 0; i < 8; i++ {
		h.h ^= b >> (8 * i) & 0xff
		h.h *= 0x100000001b3
	}
}

func (h *hash) f(v float64) { h.add(math.Float64bits(v)) }
func (h *hash) c(v complex128) {
	h.f(real(v))
	h.f(imag(v))
}
func (h *hash) b(v bool) {
	if v {
		h.add(1)
	} else {
		h.add(0)
	}
}

type diffFn func(h *hash, x, y complex128)

func one(f func(complex128) complex128) diffFn {
	return func(h *hash, x, y complex128) { h.c(f(x)) }
}

var diffs = []struct {
	name string
	fn   diffFn
}{
	{"add", func(h *hash, x, y complex128) { h.c(x + y) }},
	{"sub", func(h *hash, x, y complex128) { h.c(x - y) }},
	{"mul", func(h *hash, x, y complex128) { h.c(x * y) }},
	{"div", func(h *hash, x, y complex128) { h.c(x / y) }},
	{"abs", func(h *hash, x, y complex128) { h.f(Abs(x)) }},
	{"phase", func(h *hash, x, y complex128) { h.f(Phase(x)) }},
	{"polar", func(h *hash, x, y complex128) { r, t := Polar(x); h.f(r); h.f(t) }},
	{"rect", func(h *hash, x, y complex128) { h.c(Rect(real(x), imag(x))) }},
	{"conj", one(Conj)},
	{"is_inf", func(h *hash, x, y complex128) { h.b(IsInf(x)) }},
	{"is_nan", func(h *hash, x, y complex128) { h.b(IsNaN(x)) }},
	{"sqrt", one(Sqrt)},
	{"pow", func(h *hash, x, y complex128) { h.c(Pow(x, y)) }},
	{"exp", one(Exp)},
	{"log", one(Log)},
	{"log10", one(Log10)},
	{"sin", one(Sin)},
	{"cos", one(Cos)},
	{"tan", one(Tan)},
	{"cot", one(Cot)},
	{"asin", one(Asin)},
	{"acos", one(Acos)},
	{"atan", one(Atan)},
	{"sinh", one(Sinh)},
	{"cosh", one(Cosh)},
	{"tanh", one(Tanh)},
	{"asinh", one(Asinh)},
	{"acosh", one(Acosh)},
	{"atanh", one(Atanh)},
}

// run gives one function its inputs. The four parts of x and y cycle through
// the five kinds independently.
func run(fn diffFn) uint64 {
	d := draws{0x9e3779b97f4a7c15}
	h := hash{0xcbf29ce484222325}
	for i := 0; i < diffN; i++ {
		xr := d.float(i % 5)
		xi := d.float(i / 5 % 5)
		yr := d.float(i / 25 % 5)
		yi := d.float(i / 125 % 5)
		fn(&h, complex(xr, xi), complex(yr, yi))
	}
	return h.h
}

func cvalue(b *strings.Builder, v reflect.Value) {
	switch v.Kind() {
	case reflect.Float64:
		fmt.Fprintf(b, "UINT64_C(0x%016x)", math.Float64bits(v.Float()))
	case reflect.Complex128:
		c := v.Complex()
		fmt.Fprintf(b, "{UINT64_C(0x%016x), UINT64_C(0x%016x)}", math.Float64bits(real(c)), math.Float64bits(imag(c)))
	case reflect.Bool:
		fmt.Fprintf(b, "%t", v.Bool())
	case reflect.Array, reflect.Struct:
		b.WriteString("{")
		n := v.Len
		if v.Kind() == reflect.Struct {
			n = v.NumField
		}
		for i := 0; i < n(); i++ {
			if i > 0 {
				b.WriteString(", ")
			}
			if v.Kind() == reflect.Struct {
				cvalue(b, v.Field(i))
			} else {
				cvalue(b, v.Index(i))
			}
		}
		b.WriteString("}")
	default:
		panic(v.Kind().String())
	}
}

// ctype names the C element type of a table and the array dimension that
// goes after the name, if any. A struct is written as an array of its fields,
// which are all one type in these tables.
func ctype(t reflect.Type) (string, string) {
	switch t.Kind() {
	case reflect.Float64:
		return "uint64_t", ""
	case reflect.Complex128:
		return "Cb", ""
	case reflect.Bool:
		return "bool", ""
	case reflect.Array:
		e, _ := ctype(t.Elem())
		return e, fmt.Sprintf("[%d]", t.Len())
	case reflect.Struct:
		e, _ := ctype(t.Field(0).Type)
		return e, fmt.Sprintf("[%d]", t.NumField())
	}
	panic(t.Kind().String())
}

func TestGenCmplx(t *testing.T) {
	var b strings.Builder
	b.WriteString(`/* What Go's math/cmplx package says: the tables from the top of
 * cmath_test.go, as bit patterns, and a hash for each function of its answers
 * over the inputs tests/cmplx_test.c draws. Regenerate it with
 * tools/gen-cmplx-tests.sh rather than editing it.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

`)
	for _, tb := range tables {
		v := reflect.ValueOf(tb.v)
		et, dims := ctype(v.Type().Elem())
		fmt.Fprintf(&b, "static const %s t_%s[]%s = {\n", et, tb.name, dims)
		for i := 0; i < v.Len(); i++ {
			b.WriteString("    ")
			cvalue(&b, v.Index(i))
			b.WriteString(",\n")
		}
		b.WriteString("};\n\n")
	}
	fmt.Fprintf(&b, "#define DIFF_N %d\n\n#define DIFFS(X)", diffN)
	for _, d := range diffs {
		fmt.Fprintf(&b, " \\\n    X(%s, UINT64_C(0x%016x))", d.name, run(d.fn))
	}
	b.WriteString("\n")
	if err := os.WriteFile(os.Getenv("OUT"), []byte(b.String()), 0o644); err != nil {
		t.Fatal(err)
	}
}
