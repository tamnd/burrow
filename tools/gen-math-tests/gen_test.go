// The generator behind tests/math_test_gen.h. tools/gen-math-tests.sh copies
// it next to Go's math package and the tables from all_test.go and runs it.
//
// It writes two things. The first is every table at the top of all_test.go,
// the inputs and the answers Go's tests check against, as bit patterns so that
// nothing is lost to a decimal round trip. The second is one hash for each
// function, of what Go's portable code returns over a long run of inputs drawn
// the same way tests/math_test.c draws them. A C function whose hash matches
// gave the same bits as Go on every one of those inputs.

package math_test

import (
	"fmt"
	"os"
	"reflect"
	"strings"
	"testing"

	. "mgen/gm"
)

type table struct {
	name string
	v    any
}

// diffN is the number of inputs each function gets. The C side has the same.
const diffN = 1 << 14

type draws struct{ s uint64 }

// next is xorshift64*, which is short enough to write the same in both
// languages.
func (d *draws) next() uint64 {
	d.s ^= d.s >> 12
	d.s ^= d.s << 25
	d.s ^= d.s >> 27
	return d.s * 0x2545F4914F6CDD1D
}

// float draws a value of one of five kinds. Every conversion and product in
// here is exact, so the C side gets the same values without caring how its
// compiler rounds.
func (d *draws) float(kind int) float64 {
	r := d.next()
	switch kind {
	case 0: // any bits at all, NaNs and infinities included
		return Float64frombits(r)
	case 1: // [-1, 1)
		return float64(int64(r>>11)-(1<<52)) * 0x1p-52
	case 2: // [-32, 32)
		return float64(int64(r>>11)-(1<<52)) * 0x1p-47
	case 3: // a binary exponent in [-32, 31]
		return Float64frombits(r&0x800fffffffffffff | uint64(1023+int64(r>>52&63)-32)<<52)
	default: // halves in [-16, 16)
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

func (h *hash) f(v float64) { h.add(Float64bits(v)) }
func (h *hash) i(v int)     { h.add(uint64(int64(v))) }

func (h *hash) f32(v float32) {
	b := uint64(Float32bits(v))
	if b&0x7f800000 == 0x7f800000 && b&0x007fffff != 0 {
		b = 0x7fc00000
	}
	h.add(b)
}

type diffFn func(h *hash, x, y, z float64, n int)

func one(f func(float64) float64) diffFn {
	return func(h *hash, x, y, z float64, n int) { h.f(f(x)) }
}

func two(f func(float64, float64) float64) diffFn {
	return func(h *hash, x, y, z float64, n int) { h.f(f(x, y)) }
}

var diffs = []struct {
	name string
	fn   diffFn
}{
	{"floor", one(Floor)},
	{"ceil", one(Ceil)},
	{"trunc", one(Trunc)},
	{"round", one(Round)},
	{"round_to_even", one(RoundToEven)},
	{"modf", func(h *hash, x, y, z float64, n int) { i, f := Modf(x); h.f(i); h.f(f) }},
	{"dim", two(Dim)},
	{"max", two(Max)},
	{"min", two(Min)},
	{"mod", two(Mod)},
	{"remainder", two(Remainder)},
	{"fma", func(h *hash, x, y, z float64, n int) { h.f(FMA(x, y, z)) }},
	{"sqrt", one(Sqrt)},
	{"cbrt", one(Cbrt)},
	{"hypot", two(Hypot)},
	{"pow", two(Pow)},
	{"pow10", func(h *hash, x, y, z float64, n int) { h.f(Pow10(n%700 - 350)) }},
	{"frexp", func(h *hash, x, y, z float64, n int) { f, e := Frexp(x); h.f(f); h.i(e) }},
	{"ldexp", func(h *hash, x, y, z float64, n int) { h.f(Ldexp(x, n%2400-1200)) }},
	{"logb", one(Logb)},
	{"ilogb", func(h *hash, x, y, z float64, n int) { h.i(Ilogb(x)) }},
	{"nextafter", two(Nextafter)},
	{"nextafter32", func(h *hash, x, y, z float64, n int) { h.f32(Nextafter32(float32(x), float32(y))) }},
	{"exp", one(Exp)},
	{"exp2", one(Exp2)},
	{"expm1", one(Expm1)},
	{"log", one(Log)},
	{"log10", one(Log10)},
	{"log2", one(Log2)},
	{"log1p", one(Log1p)},
	{"sin", one(Sin)},
	{"cos", one(Cos)},
	{"tan", one(Tan)},
	{"sincos", func(h *hash, x, y, z float64, n int) { s, c := Sincos(x); h.f(s); h.f(c) }},
	{"asin", one(Asin)},
	{"acos", one(Acos)},
	{"atan", one(Atan)},
	{"atan2", two(Atan2)},
	{"sinh", one(Sinh)},
	{"cosh", one(Cosh)},
	{"tanh", one(Tanh)},
	{"asinh", one(Asinh)},
	{"acosh", one(Acosh)},
	{"atanh", one(Atanh)},
	{"erf", one(Erf)},
	{"erfc", one(Erfc)},
	{"erfinv", one(Erfinv)},
	{"erfcinv", one(Erfcinv)},
	{"gamma", one(Gamma)},
	{"lgamma", func(h *hash, x, y, z float64, n int) { l, s := Lgamma(x); h.f(l); h.i(s) }},
	{"j0", one(J0)},
	{"j1", one(J1)},
	{"jn", func(h *hash, x, y, z float64, n int) { h.f(Jn(n%16-8, x)) }},
	{"y0", one(Y0)},
	{"y1", one(Y1)},
	{"yn", func(h *hash, x, y, z float64, n int) { h.f(Yn(n%16-8, x)) }},
}

// run gives one function its inputs. x, y and z cycle through the five kinds
// independently, and n is a non-negative int each function narrows to the
// range it wants.
func run(fn diffFn) uint64 {
	d := draws{0x9e3779b97f4a7c15}
	h := hash{0xcbf29ce484222325}
	for i := 0; i < diffN; i++ {
		x := d.float(i % 5)
		y := d.float(i / 5 % 5)
		z := d.float(i / 25 % 5)
		n := int(d.next() >> 33)
		fn(&h, x, y, z, n)
	}
	return h.h
}

func cvalue(b *strings.Builder, v reflect.Value) {
	switch v.Kind() {
	case reflect.Float64:
		fmt.Fprintf(b, "UINT64_C(0x%016x)", Float64bits(v.Float()))
	case reflect.Float32:
		fmt.Fprintf(b, "UINT32_C(0x%08x)", Float32bits(float32(v.Float())))
	case reflect.Int:
		fmt.Fprintf(b, "INT64_C(%d)", v.Int())
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

// ctype names the C element type of a table.
func ctype(t reflect.Type) string {
	switch t.Kind() {
	case reflect.Float64:
		return "uint64_t"
	case reflect.Float32:
		return "uint32_t"
	case reflect.Int:
		return "int64_t"
	case reflect.Bool:
		return "bool"
	case reflect.Array:
		return ctype(t.Elem())
	case reflect.Struct:
		if t.NumField() == 2 {
			return "Fi"
		}
		return "uint64_t"
	}
	panic(t.Kind().String())
}

func TestGenMath(t *testing.T) {
	var b strings.Builder
	b.WriteString(`/* What Go's math package says: the tables from the top of all_test.go, as
 * bit patterns, and a hash for each function of its answers over the inputs
 * tests/math_test.c draws. Regenerate it with tools/gen-math-tests.sh rather
 * than editing it.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

`)
	for _, tb := range tables {
		v := reflect.ValueOf(tb.v)
		et := v.Type().Elem()
		dims := ""
		if et.Kind() == reflect.Array {
			dims = fmt.Sprintf("[%d]", et.Len())
		} else if et.Kind() == reflect.Struct && et.NumField() == 4 {
			dims = "[4]"
		}
		fmt.Fprintf(&b, "static const %s t_%s[]%s = {\n", ctype(et), tb.name, dims)
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
