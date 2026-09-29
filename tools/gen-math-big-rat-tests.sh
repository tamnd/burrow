#!/bin/sh
# Regenerates tests/big_rat_test_gen.h: a few thousand cases for math/big's
# Rat, each one an operation, its operands and what Go's math/big gives for
# it, written out as text. Every string literal in Go's rat_test.go,
# ratconv_test.go and ratmarsh_test.go goes through SetString, which picks up
# Go's tables of good and bad input, the float64 conversion stress inputs and
# the fractions its arithmetic tests use. On top of those come random
# fractions of the sizes the Int cases use, and floats near the edges of the
# denormal and overflow ranges.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/big_rat_test_gen.h"
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

// r is how an operand is written: numerator and denominator in hex.
func r(x *big.Rat) string {
	return x.Num().Text(16) + "/" + x.Denom().Text(16)
}

func exact(ok bool) string {
	if ok {
		return " exact"
	}
	return " inexact"
}

var rng = rand.New(rand.NewSource(20260929))

var sizes = []int{0, 1, 1, 1, 2, 2, 3, 4, 5, 8, 13, 20}

func random(n int, neg bool) *big.Int {
	x := new(big.Int)
	for i := 0; i < n; i++ {
		var d uint64
		switch rng.Intn(6) {
		case 0:
			d = ^uint64(0)
		case 1:
			d = 0
		case 2:
			d = 1 << uint(rng.Intn(64))
		default:
			d = rng.Uint64()
		}
		x.Lsh(x, 64)
		x.Or(x, new(big.Int).SetUint64(d))
	}
	if n > 0 && x.Sign() == 0 {
		x.SetInt64(1)
	}
	if neg {
		x.Neg(x)
	}
	return x
}

// rat is a random fraction. Now and then the two halves share a large
// factor, so that norm has something to take out.
func rat() *big.Rat {
	a := random(sizes[rng.Intn(len(sizes))], rng.Intn(2) == 0)
	b := random(sizes[rng.Intn(len(sizes))], false)
	if b.Sign() == 0 {
		b.SetInt64(1)
	}
	if rng.Intn(4) == 0 {
		f := random(1+rng.Intn(4), false)
		a.Mul(a, f)
		b.Mul(b, f)
	}
	return new(big.Rat).SetFrac(a, b)
}

// literals is every string literal in Go's own Rat tests.
func literals() []string {
	dir := filepath.Join(runtime.GOROOT(), "src", "math", "big")
	seen := map[string]bool{}
	var out []string
	for _, name := range []string{"rat_test.go", "ratconv_test.go", "ratmarsh_test.go"} {
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

func floats(x *big.Rat) {
	f, ok := x.Float64()
	emit("float64", r(x), "", "", fmt.Sprintf("%016x", math.Float64bits(f))+exact(ok))
	g, ok := x.Float32()
	emit("float32", r(x), "", "", fmt.Sprintf("%08x", math.Float32bits(g))+exact(ok))
}

func text(x *big.Rat) {
	emit("string", r(x), "", "", x.String())
	emit("ratstring", r(x), "", "", x.RatString())
	prec := rng.Intn(30)
	emit("floatstring", r(x), fmt.Sprint(prec), "", x.FloatString(prec))
	n, ok := x.FloatPrec()
	emit("floatprec", r(x), "", "", fmt.Sprint(n)+exact(ok))
	b, _ := x.GobEncode()
	emit("gobencode", r(x), "", "", hex.EncodeToString(b))
	t, _ := x.MarshalText()
	emit("marshaltext", r(x), "", "", string(t))
}

func main() {
	w = bufio.NewWriter(os.Stdout)
	defer w.Flush()

	// SetString on everything Go's tests write down, and on each of those
	// with a sign and an exponent on it.
	for _, s := range literals() {
		for _, in := range []string{s, "-" + s, s + "e-3", s + "p5"} {
			x, ok := new(big.Rat).SetString(in)
			if !ok {
				emit("setstring", in, "", "", "fail")
				continue
			}
			emit("setstring", in, "", "", x.String())
			if in == s && x.Num().BitLen()+x.Denom().BitLen() < 100000 {
				floats(x)
				text(x)
			}
		}
	}
	for _, s := range []string{"0x1p-1074", "0x1p-1075", "0x1.8p-1075", "0x1p-1076", "0x1p1023", "0x1.fffffffffffffp1023", "0x1.fffffffffffff8p1023", "0x1p1024", "0x1p-149", "0x1p-150", "0x1.8p-150", "0x1p127", "0x1.fffffep127", "0x1.ffffffp127", "0x1p128", "1e1000000", "1e1000001", "1e-1000000", "1p10000000", "1p10000001", "1p-10000000", "0b1.1p1", "0o7.7", "0x.8", "1_000.5", "1__0", "1/0x10", "0x10/1_6", "1/-2", "1/+2", " 1", "1 ", "+-1", "1/", "/1", "1//2", "1.5/2", "1e5/2"} {
		x, ok := new(big.Rat).SetString(s)
		if !ok {
			emit("setstring", s, "", "", "fail")
			continue
		}
		emit("setstring", s, "", "", x.String())
		// The limit cases are millions of bits; one parse is enough.
		if x.Num().BitLen()+x.Denom().BitLen() < 100000 {
			floats(x)
		}
	}

	// Floats: exact in, and the nearest float back out.
	for i := 0; i < 300; i++ {
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
		x := new(big.Rat).SetFloat64(f)
		bits := fmt.Sprintf("%016x", math.Float64bits(f))
		if x == nil {
			emit("setfloat64", bits, "", "", "nil")
			continue
		}
		emit("setfloat64", bits, "", "", x.String())
		floats(x)
	}
	for _, f := range []float64{0, math.Copysign(0, -1), 1, -1, 0.5, 0.1, math.MaxFloat64, math.SmallestNonzeroFloat64, math.Inf(1), math.Inf(-1), math.NaN(), math.MaxFloat32, math.SmallestNonzeroFloat32} {
		x := new(big.Rat).SetFloat64(f)
		bits := fmt.Sprintf("%016x", math.Float64bits(f))
		if x == nil {
			emit("setfloat64", bits, "", "", "nil")
			continue
		}
		emit("setfloat64", bits, "", "", x.String())
	}

	// Quotients near the edges of float64 and float32, where rounding
	// goes to a denormal, to zero or to infinity.
	for i := 0; i < 200; i++ {
		a := random(1+rng.Intn(3), rng.Intn(2) == 0)
		b := random(1+rng.Intn(3), false)
		if b.Sign() == 0 {
			b.SetInt64(1)
		}
		e := []int{-1100, -1075, -1074, -1060, -1022, -160, -150, -149, -126, 127, 128, 1000, 1023, 1024}[rng.Intn(14)]
		e += rng.Intn(8) - 4 - a.BitLen() + b.BitLen()
		if e > 0 {
			a.Lsh(a, uint(e))
		} else {
			b.Lsh(b, uint(-e))
		}
		floats(new(big.Rat).SetFrac(a, b))
	}

	// Arithmetic on random fractions.
	for i := 0; i < 400; i++ {
		x, y := rat(), rat()
		emit("add", r(x), r(y), "", new(big.Rat).Add(x, y).String())
		emit("sub", r(x), r(y), "", new(big.Rat).Sub(x, y).String())
		emit("mul", r(x), r(y), "", new(big.Rat).Mul(x, y).String())
		emit("cmp", r(x), r(y), "", fmt.Sprint(x.Cmp(y)))
		if y.Sign() != 0 {
			emit("quo", r(x), r(y), "", new(big.Rat).Quo(x, y).String())
		}
		if i%4 == 0 {
			emit("sqr", r(x), "", "", new(big.Rat).Mul(x, x).String())
			emit("cmp", r(x), r(x), "", fmt.Sprint(x.Cmp(x)))
			if x.Sign() != 0 {
				emit("inv", r(x), "", "", new(big.Rat).Inv(x).String())
			}
			emit("neg", r(x), "", "", new(big.Rat).Neg(x).String())
			emit("abs", r(x), "", "", new(big.Rat).Abs(x).String())
			floats(x)
			text(x)
		}
	}
	// FloatString and FloatPrec on fractions whose denominators have only
	// twos and fives in them, and on ones with a little more.
	for i := 0; i < 150; i++ {
		a := random(sizes[rng.Intn(len(sizes))], rng.Intn(2) == 0)
		b := new(big.Int).Exp(big.NewInt(2), big.NewInt(int64(rng.Intn(200))), nil)
		b.Mul(b, new(big.Int).Exp(big.NewInt(5), big.NewInt(int64(rng.Intn(300))), nil))
		if rng.Intn(4) == 0 {
			b.Mul(b, big.NewInt(int64(3+rng.Intn(20))))
		}
		text(new(big.Rat).SetFrac(a, b))
	}

	// SetFrac, SetFrac64, SetInt64 and SetUint64.
	for i := 0; i < 150; i++ {
		a := random(sizes[rng.Intn(len(sizes))], rng.Intn(2) == 0)
		b := random(sizes[rng.Intn(len(sizes))], rng.Intn(2) == 0)
		if b.Sign() == 0 {
			b.SetInt64(-3)
		}
		emit("setfrac", a.Text(16), b.Text(16), "", new(big.Rat).SetFrac(a, b).String())
	}
	i64 := []int64{0, 1, -1, 2, -2, 6, -6, 10, 1 << 62, math.MaxInt64, math.MinInt64, math.MinInt64 + 1, 1e18, -1e18}
	for _, a := range i64 {
		emit("setint64", fmt.Sprint(a), "", "", new(big.Rat).SetInt64(a).String())
		emit("setuint64", fmt.Sprint(uint64(a)), "", "", new(big.Rat).SetUint64(uint64(a)).String())
		for _, b := range i64 {
			if b != 0 {
				emit("setfrac64", fmt.Sprint(a), fmt.Sprint(b), "", new(big.Rat).SetFrac64(a, b).String())
			}
		}
	}

	// GobDecode on bad and odd input, and on what GobEncode wrote.
	for _, s := range []string{"", "02", "0200000000", "02000000ff", "02ffffffff", "0200000001", "020000000105", "030000000105", "04000000010203", "0000000000", "0200000000ff", "0200000002000101"} {
		b, _ := hex.DecodeString(s)
		x := big.NewRat(1, 2)
		err := x.GobDecode(b)
		want := r(x)
		if err != nil {
			want = "err:" + err.Error()
		}
		emit("gobdecode", s, "", "", want)
	}
	// Scanning through fmt.
	for _, t := range [][2]string{{"1/2", "%v"}, {"  -3.25e2 rest", "%v"}, {"7/21", "%f"}, {"1e-3", "%g"}, {"2.5", "%E"}, {"abc", "%v"}, {"1/0", "%v"}, {"1/2", "%d"}, {"1/2", "%s"}, {"0x1p-2", "%v"}, {"--1", "%v"}, {"", "%v"}} {
		x := big.NewRat(5, 1)
		_, err := fmt.Sscanf(t[0], t[1], x)
		want := r(x)
		if err != nil {
			want = "err:" + err.Error()
		}
		emit("scan", t[0], t[1], "", want)
	}
	for _, s := range []string{"1/2", "-3", "0", "abc", "1/0", "0x10", "1e3", " 2"} {
		x := big.NewRat(7, 9)
		err := x.UnmarshalText([]byte(s))
		want := r(x)
		if err != nil {
			want = "err:" + err.Error()
		}
		emit("unmarshaltext", s, "", "", want)
	}
}
GO

cd "$tmp"
go mod init ratgen > /dev/null 2>&1
go run . > "$tmp/cases.inc"

{
	cat <<'HDR'
/* What Go's math/big says about a few thousand Rat operations, one row each:
 * the operation, up to three operands and the result, as text. An operand
 * Rat is its numerator and denominator in hex with a / between them. Results
 * are what Go prints. Regenerate it with tools/gen-math-big-rat-tests.sh rather
 * than editing it.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

typedef struct RatCase {
    const char *op;
    const char *a;
    const char *b;
    const char *c;
    const char *want;
} RatCase;

/* clang-format off */
static const RatCase rat_cases[] = {
HDR
	cat "$tmp/cases.inc"
	printf '};\n/* clang-format on */\n'
} > "$out"
