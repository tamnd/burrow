#!/bin/sh
# Regenerates tests/big_float_test_gen.h: several thousand cases for math/big's
# Float, each one an operation, its operands and what Go's math/big gives for
# it, written out as text. Every string literal in Go's float_test.go,
# floatconv_test.go and floatmarsh_test.go goes through Parse in a few bases
# and precisions, which picks up Go's tables of good and bad input. On top of
# those come random Floats at many precisions and all six rounding modes,
# through the arithmetic, the conversions and the text formats.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/big_float_test_gen.h"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/main.go" <<'GO'
package main

import (
	"bufio"
	"encoding/hex"
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"hash/fnv"
	"math"
	"math/big"
	"math/rand"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
)

var w *bufio.Writer

func cstr(s string) string {
	var b strings.Builder
	b.WriteByte('"')
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c < 0x20 || c >= 0x7f || c == '"' || c == '\\' || c == '?' {
			fmt.Fprintf(&b, "\\%03o", c)
		} else {
			b.WriteByte(c)
		}
		if i%2000 == 1999 && i+1 < len(s) {
			b.WriteString("\"\n    \"")
		}
	}
	b.WriteByte('"')
	return b.String()
}

// emit writes one case. A result longer than 120 bytes is kept as its
// FNV-1a hash, written as # and 16 hex digits.
func emit(op, a, b, c, want string) {
	if len(want) > 120 {
		f := fnv.New64a()
		f.Write([]byte(want))
		want = fmt.Sprintf("#%016x", f.Sum64())
	}
	fmt.Fprintf(w, "    {%s, %s, %s, %s, %s},\n", cstr(op), cstr(a), cstr(b), cstr(c), cstr(want))
}

// enc is how an operand is written: precision, rounding mode and the exact
// value in the 'p' format, which Parse reads back with base 0.
func enc(x *big.Float) string {
	return fmt.Sprintf("%d/%d/%s", x.Prec(), x.Mode(), x.Text('p', 0))
}

// dec reads an operand back the way the C side does.
func dec(s string) *big.Float {
	parts := strings.SplitN(s, "/", 3)
	prec, _ := strconv.Atoi(parts[0])
	mode, _ := strconv.Atoi(parts[1])
	z := new(big.Float).SetMode(big.RoundingMode(mode))
	if prec > 0 {
		z.SetPrec(uint(prec))
	}
	if _, _, err := z.Parse(parts[2], 0); err != nil {
		panic(s + ": " + err.Error())
	}
	if prec == 0 {
		z.SetPrec(0)
	}
	return z
}

// res is how a result is written: enc with the accuracy in the middle.
func res(z *big.Float) string {
	return fmt.Sprintf("%d/%d/%d/%s", z.Prec(), z.Mode(), z.Acc(), z.Text('p', 0))
}

// recv is a receiver of the precision and mode written in c.
func recv(c string) *big.Float {
	parts := strings.SplitN(c, "/", 2)
	prec, _ := strconv.Atoi(parts[0])
	mode, _ := strconv.Atoi(parts[1])
	z := new(big.Float).SetMode(big.RoundingMode(mode))
	if prec > 0 {
		z.SetPrec(uint(prec))
	}
	return z
}

// try runs f and turns an ErrNaN panic into "nan:" and its message.
func try(f func() string) (s string) {
	defer func() {
		if r := recover(); r != nil {
			s = "nan:" + r.(big.ErrNaN).Error()
		}
	}()
	return f()
}

var rng = rand.New(rand.NewSource(20260930))

var precs = []uint{1, 2, 3, 7, 10, 24, 32, 53, 63, 64, 65, 100, 128, 129, 200, 256, 500, 1000}

func prec() uint {
	return precs[rng.Intn(len(precs))]
}

func mode() big.RoundingMode {
	return big.RoundingMode(rng.Intn(6))
}

// zc is a receiver setting: precision 0 (take it from the operands) a third
// of the time.
func zc() string {
	p := uint(0)
	if rng.Intn(3) != 0 {
		p = prec()
	}
	return fmt.Sprintf("%d/%d", p, mode())
}

// random is a finite Float with a random mantissa, or now and then a zero or
// an infinity. Exponents stay within span of zero.
func random(span int) *big.Float {
	p := prec()
	x := new(big.Float).SetPrec(p).SetMode(mode())
	switch rng.Intn(20) {
	case 0:
		x.SetInt64(0)
		if rng.Intn(2) == 0 {
			x.Neg(x)
		}
		return dec(enc(x))
	case 1:
		x.SetInf(rng.Intn(2) == 0)
		return dec(enc(x))
	}
	m := new(big.Int)
	bits := int(p) + rng.Intn(40) - 20
	if bits < 1 {
		bits = 1
	}
	for m.BitLen() < bits {
		var d uint64
		switch rng.Intn(5) {
		case 0:
			d = ^uint64(0)
		case 1:
			d = 1 << uint(rng.Intn(64))
		default:
			d = rng.Uint64()
		}
		m.Lsh(m, 64)
		m.Or(m, new(big.Int).SetUint64(d))
	}
	if m.Sign() == 0 {
		m.SetInt64(1)
	}
	if rng.Intn(2) == 0 {
		m.Neg(m)
	}
	x.SetInt(m)
	e := rng.Intn(2*span+1) - span
	x.SetMantExp(x, e-x.MantExp(nil))
	return dec(enc(x))
}

// literals is every string literal in Go's own Float tests.
func literals() []string {
	dir := filepath.Join(runtime.GOROOT(), "src", "math", "big")
	seen := map[string]bool{}
	var out []string
	for _, name := range []string{"float_test.go", "floatconv_test.go", "floatmarsh_test.go"} {
		f, err := parser.ParseFile(token.NewFileSet(), filepath.Join(dir, name), nil, 0)
		if err != nil {
			panic(err)
		}
		ast.Inspect(f, func(n ast.Node) bool {
			if l, ok := n.(*ast.BasicLit); ok && l.Kind == token.STRING {
				s, err := strconv.Unquote(l.Value)
				if err == nil && !strings.Contains(s, "%") && !seen[s] && len(s) < 4000 {
					seen[s] = true
					out = append(out, s)
				}
			}
			return true
		})
	}
	sort.Strings(out)
	return out
}

func accs(a big.Accuracy) string {
	return fmt.Sprint(int(a))
}

func binop(op string, x, y *big.Float, f func(z, x, y *big.Float) *big.Float) {
	c := zc()
	emit(op, enc(x), enc(y), c, try(func() string { return res(f(recv(c), x, y)) }))
}

func unop(op string, x *big.Float, f func(z, x *big.Float) *big.Float) {
	c := zc()
	emit(op, enc(x), "", c, try(func() string { return res(f(recv(c), x)) }))
}

// small reports whether x's decimal digits are few enough to print.
func small(x *big.Float) bool {
	e := x.MantExp(nil)
	return e > -3000 && e < 3000
}

func conv(x *big.Float) {
	u, a := x.Uint64()
	emit("uint64", enc(x), "", "", fmt.Sprint(u)+" "+accs(a))
	i, a := x.Int64()
	emit("int64", enc(x), "", "", fmt.Sprint(i)+" "+accs(a))
	f32, a := x.Float32()
	emit("float32", enc(x), "", "", fmt.Sprintf("%08x %s", math.Float32bits(f32), accs(a)))
	f64, a := x.Float64()
	emit("float64", enc(x), "", "", fmt.Sprintf("%016x %s", math.Float64bits(f64), accs(a)))
	if small(x) {
		n, a := x.Int(new(big.Int))
		s := "nil"
		if n != nil {
			s = n.Text(16)
		}
		emit("int", enc(x), "", "", s+" "+accs(a))
		r, a := x.Rat(new(big.Rat))
		s = "nil"
		if r != nil {
			s = r.String()
		}
		emit("rat", enc(x), "", "", s+" "+accs(a))
	}
}

func info(x *big.Float) {
	m := new(big.Float)
	e := x.MantExp(m)
	emit("mantexp", enc(x), "", "", fmt.Sprint(e)+" "+res(m))
	emit("minprec", enc(x), "", "", fmt.Sprint(x.MinPrec()))
	emit("sign", enc(x), "", "", fmt.Sprint(x.Sign()))
	emit("isint", enc(x), "", "", fmt.Sprint(x.IsInt()))
	p := prec()
	if rng.Intn(8) == 0 {
		p = 0
	}
	emit("setprec", enc(x), fmt.Sprint(p), "", res(new(big.Float).Copy(x).SetPrec(p)))
	m2 := rng.Intn(6)
	emit("setmode", enc(x), fmt.Sprint(m2), "", res(new(big.Float).Copy(x).SetMode(big.RoundingMode(m2))))
}

var formats = []byte{'b', 'e', 'E', 'f', 'g', 'G', 'p', 'x', 'q'}

func text(x *big.Float) {
	for i := 0; i < 3; i++ {
		f := formats[rng.Intn(len(formats))]
		if !small(x) && f != 'b' && f != 'p' && f != 'x' {
			continue
		}
		p := []int{-1, -1, 0, 1, 2, 3, 5, 8, 10, 16, 17, 25, 40}[rng.Intn(13)]
		emit("text", enc(x), string(f), fmt.Sprint(p), x.Text(f, p))
	}
	if small(x) {
		emit("string", enc(x), "", "", x.String())
		t, _ := x.MarshalText()
		emit("marshaltext", enc(x), "", "", string(t))
	}
	b, _ := x.GobEncode()
	emit("gobencode", enc(x), "", "", hex.EncodeToString(b))
}

var verbs = []string{"%v", "%s", "%d", "%g", "%G", "%e", "%E", "%f", "%F", "%b", "%x", "%.3f", "%10.2e", "%-12g|", "%+g", "% g", "%010.3f", "%+.5x", "%20v|", "%-20.4F|", "%08g", "%+010e", "% 12.1f", "%.0e", "%.0f", "%#g"}

func sprintf(x *big.Float) {
	v := verbs[rng.Intn(len(verbs))]
	if !small(x) && !strings.ContainsAny(v, "bx") {
		return
	}
	emit("sprintf", enc(x), v, "", fmt.Sprintf(v, x))
}

func main() {
	w = bufio.NewWriter(os.Stdout)
	defer w.Flush()

	// Parse on everything Go's tests write down, in the bases that make
	// sense, at its own precision and at a random one.
	for _, s := range literals() {
		for _, base := range []int{0, 10, 16} {
			if base == 16 && rng.Intn(3) != 0 {
				continue
			}
			for _, c := range []string{"0/0", zc()} {
				z := recv(c)
				_, b, err := z.Parse(s, base)
				want := ""
				if err != nil {
					want = "err:" + err.Error()
				} else {
					want = res(z) + " " + fmt.Sprint(b)
				}
				emit("parse", s, fmt.Sprint(base), c, want)
			}
		}
		if x, _, err := big.ParseFloat(s, 0, 0, big.ToNearestEven); err == nil && small(x) {
			x = dec(enc(x))
			conv(x)
			text(x)
		}
	}
	for _, s := range []string{"Inf", "inf", "+Inf", "-inf", "INF", "infinity", "0x1p2147483647", "0x1p2147483648", "0x1p-2147483648", "0x1p-2147483649", "1e646456992", "1e646456993", "1e-646456992", "1e-646456993", "0b1.1p1", "0o7.7", "0x.8", "1_000.5", "1__0", "0x1.8p-1", "-0", "+0", ".5", "5.", "1e", "1p", "0x", "--1", " 1", "1 ", "0.1e+1_0", "0e1000000000000"} {
		for _, base := range []int{0, 10} {
			c := zc()
			z := recv(c)
			_, b, err := z.Parse(s, base)
			want := ""
			if err != nil {
				want = "err:" + err.Error()
			} else {
				want = res(z) + " " + fmt.Sprint(b)
			}
			emit("parse", s, fmt.Sprint(base), c, want)
		}
	}

	// Arithmetic on random Floats.
	for i := 0; i < 900; i++ {
		x, y := random(200), random(200)
		if rng.Intn(4) == 0 {
			// close in size, so that subtraction cancels
			y = new(big.Float).SetPrec(y.Prec()).SetMode(y.Mode()).Set(x)
			y.SetMantExp(y, 0)
			d := random(5)
			if d.IsInf() {
				d.SetInt64(1)
			}
			d.SetMantExp(d, x.MantExp(nil)-int(x.Prec())-rng.Intn(10))
			y.Add(y, d)
			y = dec(enc(y))
		}
		binop("add", x, y, (*big.Float).Add)
		binop("sub", x, y, (*big.Float).Sub)
		binop("mul", x, y, (*big.Float).Mul)
		binop("quo", x, y, (*big.Float).Quo)
		emit("cmp", enc(x), enc(y), "", fmt.Sprint(x.Cmp(y)))
		if i%3 == 0 {
			binop("add", x, x, (*big.Float).Add)
			binop("mul", x, x, (*big.Float).Mul)
			binop("quo", x, x, (*big.Float).Quo)
			unop("sqrt", x, (*big.Float).Sqrt)
			unop("neg", x, (*big.Float).Neg)
			unop("abs", x, (*big.Float).Abs)
			unop("set", x, (*big.Float).Set)
			unop("copy", x, (*big.Float).Copy)
			info(x)
			conv(x)
			text(x)
			sprintf(x)
		}
	}

	// Near the ends of the exponent range, where results overflow to an
	// infinity or underflow to zero.
	for i := 0; i < 150; i++ {
		x, y := random(20), random(20)
		ex := []int{math.MaxInt32, math.MaxInt32 - 1, math.MinInt32, math.MinInt32 + 1, 1 << 30, -1 << 30}[rng.Intn(6)]
		ey := ex
		if rng.Intn(2) == 0 {
			ey = -ex
		}
		if !x.IsInf() && x.Sign() != 0 {
			x.SetMantExp(x, ex-x.MantExp(nil))
		}
		if !y.IsInf() && y.Sign() != 0 {
			y.SetMantExp(y, ey/2+rng.Intn(3)-1-y.MantExp(nil))
		}
		x, y = dec(enc(x)), dec(enc(y))
		binop("mul", x, y, (*big.Float).Mul)
		binop("quo", x, y, (*big.Float).Quo)
		binop("mul", x, x, (*big.Float).Mul)
		unop("sqrt", x, (*big.Float).Sqrt)
		emit("cmp", enc(x), enc(y), "", fmt.Sprint(x.Cmp(y)))
		info(x)
		conv(x)
		text(x)
		sprintf(x)
		e := []int{1, -1, 5, -5, math.MaxInt32, math.MinInt32, 1 << 31, -1 << 31}[rng.Intn(8)]
		c := zc()
		emit("setmantexp", enc(x), fmt.Sprint(e), c, res(recv(c).SetMantExp(x, e)))
	}

	// Conversions near the edges of the integer and float types.
	for i := 0; i < 300; i++ {
		x := random(0)
		e := []int{0, 1, 31, 32, 33, 52, 53, 54, 63, 64, 65, -126, -127, -148, -149, -150, 127, 128, 129, -1021, -1022, -1073, -1074, -1075, 1023, 1024, 1025}[rng.Intn(27)]
		if !x.IsInf() && x.Sign() != 0 {
			x.SetMantExp(x, e+rng.Intn(3)-1-x.MantExp(nil))
		}
		conv(dec(enc(x)))
	}
	for _, s := range []string{"-0x.8p+64", "-0x.8p+64", "0x.8p+64", "0x.ffffffffffffffffp+64", "0x.8p+63", "-0x.8p+63", "0x.ffffffp+128", "0x.ffffff8p+128", "0x.8p-148", "0x.8p-149", "0x.cp-149", "0x.8p-1073", "0x.8p-1074", "0x.cp-1074", "0x.fffffffffffff8p+1024", "0x.fffffffffffffcp+1024"} {
		for _, p := range []int{0, 1, 2, 24, 53, 64, 100} {
			if p == 0 {
				conv(dec("200/0/" + s))
			} else {
				x := dec("200/" + fmt.Sprint(rng.Intn(6)) + "/" + s)
				x.SetPrec(uint(p))
				conv(dec(enc(x)))
			}
		}
	}

	// The setters.
	i64 := []int64{0, 1, -1, 2, -3, 1 << 53, 1<<53 + 1, 1 << 62, math.MaxInt64, math.MinInt64, math.MinInt64 + 1, 1e18, -1e18, 12345678901}
	for _, x := range i64 {
		for j := 0; j < 3; j++ {
			c := zc()
			emit("setint64", fmt.Sprint(x), "", c, res(recv(c).SetInt64(x)))
			emit("setuint64", fmt.Sprint(uint64(x)), "", c, res(recv(c).SetUint64(uint64(x))))
		}
	}
	for i := 0; i < 250; i++ {
		var f float64
		switch rng.Intn(4) {
		case 0:
			f = math.Float64frombits(rng.Uint64())
		case 1:
			f = math.Float64frombits(rng.Uint64() & (1<<52 - 1)) // denormal
		case 2:
			f = float64(math.Float32frombits(rng.Uint32()))
		default:
			f = rng.NormFloat64() * math.Pow(10, float64(rng.Intn(40)-20))
		}
		c := zc()
		bits := fmt.Sprintf("%016x", math.Float64bits(f))
		emit("setfloat64", bits, "", c, try(func() string { return res(recv(c).SetFloat64(f)) }))
	}
	for _, f := range []float64{0, math.Copysign(0, -1), 1, -1, 0.5, 0.1, math.MaxFloat64, math.SmallestNonzeroFloat64, math.Inf(1), math.Inf(-1), math.NaN()} {
		c := zc()
		bits := fmt.Sprintf("%016x", math.Float64bits(f))
		emit("setfloat64", bits, "", c, try(func() string { return res(recv(c).SetFloat64(f)) }))
	}
	for i := 0; i < 200; i++ {
		n := new(big.Int).Rand(rng, new(big.Int).Lsh(big.NewInt(1), uint(1+rng.Intn(600))))
		if rng.Intn(2) == 0 {
			n.Neg(n)
		}
		c := zc()
		emit("setint", n.Text(16), "", c, res(recv(c).SetInt(n)))
		d := new(big.Int).Rand(rng, new(big.Int).Lsh(big.NewInt(1), uint(1+rng.Intn(300))))
		d.Add(d, big.NewInt(1))
		r := new(big.Rat).SetFrac(n, d)
		c = zc()
		emit("setrat", r.Num().Text(16)+"/"+r.Denom().Text(16), "", c, res(recv(c).SetRat(r)))
	}

	// NaN results, which panic.
	pinf, ninf := new(big.Float).SetInf(false), new(big.Float).SetInf(true)
	zero, one := new(big.Float), big.NewFloat(1)
	binop("add", pinf, ninf, (*big.Float).Add)
	binop("sub", pinf, pinf, (*big.Float).Sub)
	binop("mul", zero, pinf, (*big.Float).Mul)
	binop("mul", ninf, zero, (*big.Float).Mul)
	binop("quo", zero, zero, (*big.Float).Quo)
	binop("quo", ninf, pinf, (*big.Float).Quo)
	binop("quo", one, zero, (*big.Float).Quo)
	unop("sqrt", new(big.Float).Neg(one), (*big.Float).Sqrt)
	unop("sqrt", ninf, (*big.Float).Sqrt)

	// Sqrt at many precisions.
	for i := 0; i < 150; i++ {
		x := random(100)
		x.Abs(x)
		unop("sqrt", dec(enc(x)), (*big.Float).Sqrt)
	}

	// GobDecode on bad and odd input, and on what GobEncode wrote.
	for _, s := range []string{"", "01", "0100000000", "02000000000000", "0102", "010200000040", "0102000000400000000180", "01020000004000000001", "010200000040000000018000000000000000", "01020000004000000001000000000000000080", "0102000000000000000180000000000000", "01040000004000", "01060000004000", "01e20000004000000001c000000000000000"} {
		for _, c := range []string{"0/0", "10/2"} {
			b, _ := hex.DecodeString(s)
			z := recv(c)
			z.SetInt64(5)
			err := z.GobDecode(b)
			want := res(z)
			if err != nil {
				want = "err:" + err.Error()
			}
			emit("gobdecode", s, "", c, want)
		}
	}
	// Scanning through fmt.
	for _, t := range [][2]string{{"1.5", "%v"}, {"  -3.25e2 rest", "%v"}, {"0x1p-2", "%g"}, {"1e", "%v"}, {"abc", "%v"}, {"Inf", "%v"}, {"1_000", "%v"}, {"0b101", "%f"}, {"", "%v"}, {"--1", "%v"}, {"7", "%d"}} {
		x := big.NewFloat(5)
		_, err := fmt.Sscanf(t[0], t[1], x)
		want := res(x)
		if err != nil {
			want = "err:" + err.Error()
		}
		emit("scan", t[0], t[1], "", want)
	}
	for _, s := range []string{"1.5", "-3", "0", "abc", "Inf", "-inf", "0x10p-2", "1e3", " 2", "1e", ""} {
		for _, c := range []string{"0/0", "10/1"} {
			x := recv(c)
			err := x.UnmarshalText([]byte(s))
			want := res(x)
			if err != nil {
				want = "err:" + err.Error()
			}
			emit("unmarshaltext", s, "", c, want)
		}
	}
}
GO

cd "$tmp"
go mod init floatgen > /dev/null 2>&1
go run . > "$tmp/cases.inc"

{
	cat <<'HDR'
/* What Go's math/big says about several thousand Float operations, one row
 * each: the operation, up to three operands and the result, as text. An
 * operand Float is its precision, rounding mode and value in the 'p' format,
 * with a / between them, and a result has its accuracy after the mode. A
 * receiver is a precision and a mode. Regenerate it with
 * tools/gen-math-big-float-tests.sh rather than editing it.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

typedef struct FloatCase {
    const char *op;
    const char *a;
    const char *b;
    const char *c;
    const char *want;
} FloatCase;

/* clang-format off */
static const FloatCase float_cases[] = {
HDR
	cat "$tmp/cases.inc"
	printf '};\n/* clang-format on */\n'
} > "$out"
