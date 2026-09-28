#!/bin/sh
# Regenerates tests/big_test_gen.h: a few thousand cases for math/big's Int,
# each one an operation, its operands and what Go's math/big gives for it,
# written out as text. The operands are random numbers of the sizes that reach
# every algorithm the package switches between: single words, the basic loops,
# Karatsuba, recursive division, the windowed and Montgomery exponentiation
# and the divisor tables that convert long numbers to text. On top of those
# come the fixed tables from Go's int_test.go and intconv_test.go that the
# random cases would not hit, such as prefixes, separators and bad input.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/big_test_gen.h"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/main.go" <<'GO'
package main

import (
	"bufio"
	"encoding/hex"
	"fmt"
	"hash/fnv"
	"math"
	"math/big"
	"math/rand"
	"os"
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
		// Keep lines, and string literals, a sensible length.
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

func h(x *big.Int) string {
	if x == nil {
		return "nil"
	}
	return x.Text(16)
}

var rng = rand.New(rand.NewSource(20260928))

var sizes = []int{0, 1, 1, 1, 2, 2, 3, 4, 5, 8, 13, 20}

// random is a number of n 64 bit words, sometimes with long runs of ones or
// zeros, which is where carries and corrections go wrong.
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

func rnd() *big.Int {
	return random(sizes[rng.Intn(len(sizes))], rng.Intn(2) == 0)
}

func rndPos() *big.Int {
	return random(sizes[rng.Intn(len(sizes))], false)
}

func pair() (*big.Int, *big.Int) {
	return rnd(), rnd()
}

func binop(op string, n int, f func(z, x, y *big.Int) *big.Int, nonzero bool) {
	for i := 0; i < n; i++ {
		x, y := pair()
		if nonzero && y.Sign() == 0 {
			y.SetInt64(7)
		}
		emit(op, h(x), h(y), "", h(f(new(big.Int), x, y)))
	}
}

func main() {
	w = bufio.NewWriter(os.Stdout)
	defer w.Flush()

	binop("add", 150, (*big.Int).Add, false)
	binop("sub", 150, (*big.Int).Sub, false)
	binop("mul", 200, (*big.Int).Mul, false)
	for i := 0; i < 30; i++ {
		x := random(40+rng.Intn(100), rng.Intn(2) == 0)
		y := random(40+rng.Intn(100), rng.Intn(2) == 0)
		emit("mul", h(x), h(y), "", h(new(big.Int).Mul(x, y)))
		emit("sqr", h(x), "", "", h(new(big.Int).Mul(x, x)))
	}
	for i := 0; i < 60; i++ {
		x := rnd()
		emit("sqr", h(x), "", "", h(new(big.Int).Mul(x, x)))
	}
	binop("quo", 150, (*big.Int).Quo, true)
	binop("rem", 150, (*big.Int).Rem, true)
	binop("div", 150, (*big.Int).Div, true)
	binop("mod", 150, (*big.Int).Mod, true)
	// Long by long, for the recursive division.
	for i := 0; i < 40; i++ {
		x := random(80+rng.Intn(220), rng.Intn(2) == 0)
		y := random(41+rng.Intn(80), rng.Intn(2) == 0)
		q, r := new(big.Int).QuoRem(x, y, new(big.Int))
		emit("quorem", h(x), h(y), "", h(q)+" "+h(r))
	}
	for i := 0; i < 150; i++ {
		x, y := pair()
		if y.Sign() == 0 {
			y.SetInt64(-3)
		}
		q, r := new(big.Int).QuoRem(x, y, new(big.Int))
		emit("quorem", h(x), h(y), "", h(q)+" "+h(r))
		q, m := new(big.Int).DivMod(x, y, new(big.Int))
		emit("divmod", h(x), h(y), "", h(q)+" "+h(m))
		for _, mode := range []struct {
			name string
			m    big.RoundingMode
		}{{"T", big.ToZero}, {"F", big.ToNegativeInf}, {"R", big.ToNearestEven}, {"C", big.ToPositiveInf}} {
			q, r := new(big.Int).Divide(x, y, new(big.Int), mode.m)
			emit("divide", h(x), h(y), mode.name, h(q)+" "+h(r))
		}
	}
	// Ties for R-division.
	for i := 0; i < 40; i++ {
		y := random(1+rng.Intn(3), rng.Intn(2) == 0)
		y.Lsh(y, 1)
		q := rnd()
		x := new(big.Int).Mul(q, y)
		x.Add(x, new(big.Int).Rsh(new(big.Int).Abs(y), 1))
		qq, r := new(big.Int).Divide(x, y, new(big.Int), big.ToNearestEven)
		emit("divide", h(x), h(y), "R", h(qq)+" "+h(r))
	}
	for i := 0; i < 150; i++ {
		x, y := pair()
		if i%5 == 0 {
			y.Set(x)
		}
		if i%7 == 0 {
			y.Neg(x)
		}
		emit("cmp", h(x), h(y), "", fmt.Sprint(x.Cmp(y)))
		emit("cmpabs", h(x), h(y), "", fmt.Sprint(x.CmpAbs(y)))
	}
	for i := 0; i < 150; i++ {
		x := rnd()
		s := uint(rng.Intn(300))
		if i%3 == 0 {
			s = uint(rng.Intn(64))
		}
		emit("lsh", h(x), fmt.Sprint(s), "", h(new(big.Int).Lsh(x, s)))
		emit("rsh", h(x), fmt.Sprint(s), "", h(new(big.Int).Rsh(x, s)))
	}
	binop("and", 150, (*big.Int).And, false)
	binop("andnot", 150, (*big.Int).AndNot, false)
	binop("or", 150, (*big.Int).Or, false)
	binop("xor", 150, (*big.Int).Xor, false)
	for i := 0; i < 100; i++ {
		x := rnd()
		emit("not", h(x), "", "", h(new(big.Int).Not(x)))
		emit("neg", h(x), "", "", h(new(big.Int).Neg(x)))
		emit("abs", h(x), "", "", h(new(big.Int).Abs(x)))
		emit("bitlen", h(x), "", "", fmt.Sprint(x.BitLen()))
		emit("tzb", h(x), "", "", fmt.Sprint(x.TrailingZeroBits()))
		ax := new(big.Int).Abs(x)
		emit("sqrt", h(ax), "", "", h(new(big.Int).Sqrt(ax)))
		i := rng.Intn(x.BitLen() + 70)
		emit("bit", h(x), fmt.Sprint(i), "", fmt.Sprint(x.Bit(i)))
		b := uint(rng.Intn(2))
		emit("setbit", h(x), fmt.Sprint(i), fmt.Sprint(b), h(new(big.Int).SetBit(x, i, b)))
	}
	// Squares and their neighbours, for Sqrt.
	for i := 0; i < 40; i++ {
		x := rndPos()
		sq := new(big.Int).Mul(x, x)
		for _, d := range []int64{-1, 0, 1} {
			v := new(big.Int).Add(sq, big.NewInt(d))
			if v.Sign() >= 0 {
				emit("sqrt", h(v), "", "", h(new(big.Int).Sqrt(v)))
			}
		}
	}

	// Exp, with every kind of modulus: none, zero, one word, odd for
	// Montgomery, even, a power of two, and long enough for the window.
	for i := 0; i < 250; i++ {
		x := random(sizes[rng.Intn(10)], rng.Intn(3) == 0)
		y := random(rng.Intn(4), rng.Intn(6) == 0)
		var m *big.Int
		switch i % 6 {
		case 0:
			m = nil
			y = big.NewInt(int64(rng.Intn(40)))
			if rng.Intn(4) == 0 {
				y.Neg(y)
			}
		case 1:
			m = new(big.Int)
			y = big.NewInt(int64(rng.Intn(40)))
		case 2:
			m = random(1, rng.Intn(2) == 0)
		case 3:
			m = random(1+rng.Intn(20), rng.Intn(2) == 0)
			m.SetBit(m, 0, 1)
		case 4:
			m = random(1+rng.Intn(20), rng.Intn(2) == 0)
			m.SetBit(m, 0, 0)
			if m.Sign() == 0 {
				m.SetInt64(6)
			}
		case 5:
			m = new(big.Int).Lsh(big.NewInt(1), uint(rng.Intn(400)))
			if rng.Intn(2) == 0 {
				m.Mul(m, big.NewInt(int64(2*rng.Intn(1000)+1)))
			}
		}
		if i%6 != 0 && m.Sign() != 0 && rng.Intn(3) == 0 {
			y = random(1+rng.Intn(20), false)
		}
		z := new(big.Int).Exp(x, y, m)
		ms := ""
		if m != nil {
			ms = h(m)
		}
		emit("exp", h(x), h(y), ms, h(z))
	}

	for i := 0; i < 30; i++ {
		a := random(20+rng.Intn(60), rng.Intn(2) == 0)
		b := random(20+rng.Intn(60), rng.Intn(2) == 0)
		x, y := new(big.Int), new(big.Int)
		z := new(big.Int).GCD(x, y, a, b)
		emit("gcd", h(a), h(b), "", h(z)+" "+h(x)+" "+h(y))
	}
	for i := 0; i < 20; i++ {
		x := random(40+rng.Intn(100), false)
		emit("sqrt", h(x), "", "", h(new(big.Int).Sqrt(x)))
	}
	for i := 0; i < 200; i++ {
		a, b := pair()
		if i%10 == 0 {
			a.SetInt64(0)
		}
		if i%10 == 1 {
			b.SetInt64(0)
		}
		if i%10 == 2 {
			c := rndPos()
			a.Mul(a, c)
			b.Mul(b, c)
		}
		x, y := new(big.Int), new(big.Int)
		z := new(big.Int).GCD(x, y, a, b)
		emit("gcd", h(a), h(b), "", h(z)+" "+h(x)+" "+h(y))
		emit("gcdz", h(a), h(b), "", h(new(big.Int).GCD(nil, nil, a, b)))
	}
	for i := 0; i < 150; i++ {
		g, n := pair()
		if n.Sign() == 0 {
			n.SetInt64(97)
		}
		emit("modinv", h(g), h(n), "", h(new(big.Int).ModInverse(g, n)))
	}
	for i := 0; i < 150; i++ {
		x := rnd()
		y := rnd()
		if y.Sign() == 0 {
			y.SetInt64(1)
		}
		y.SetBit(y, 0, 1)
		if y.Sign() < 0 {
			y.Neg(y)
			y.SetBit(y, 0, 1)
			y.Neg(y)
		}
		emit("jacobi", h(x), h(y), "", fmt.Sprint(big.Jacobi(x, y)))
	}
	// ModSqrt with primes of each residue the three algorithms take.
	for i := 0; i < 120; i++ {
		var p *big.Int
		for {
			p = random(1+rng.Intn(4), false)
			p.SetBit(p, 0, 1)
			switch i % 3 {
			case 0: // 3 mod 4
				p.SetBit(p, 1, 1)
			case 1: // 5 mod 8
				p.SetBit(p, 1, 0)
				p.SetBit(p, 2, 1)
			case 2: // 1 mod 8
				p.SetBit(p, 1, 0)
				p.SetBit(p, 2, 0)
				p.SetBit(p, 3+rng.Intn(20), 0)
			}
			if p.ProbablyPrime(20) {
				break
			}
		}
		x := random(rng.Intn(5), rng.Intn(4) == 0)
		if i%2 == 0 {
			x.Mul(x, x)
		}
		emit("modsqrt", h(x), h(p), "", h(new(big.Int).ModSqrt(x, p)))
	}

	// ProbablyPrime: small numbers, random odd ones, primes, and the strong
	// pseudoprimes and Lucas pseudoprimes Go's prime_test.go lists.
	for i := 0; i < 200; i++ {
		emit("prime", h(big.NewInt(int64(i))), "0", "", fmt.Sprint(big.NewInt(int64(i)).ProbablyPrime(0)))
	}
	for i := 0; i < 200; i++ {
		x := random(1+rng.Intn(8), false)
		x.SetBit(x, 0, 1)
		n := rng.Intn(20)
		emit("prime", h(x), fmt.Sprint(n), "", fmt.Sprint(x.ProbablyPrime(n)))
	}
	for i := 0; i < 40; i++ {
		x := random(1+rng.Intn(8), false)
		p := new(big.Int)
		for p.Set(x); !p.ProbablyPrime(20); p.Add(p, big.NewInt(1)) {
		}
		emit("prime", h(p), "10", "", "true")
		q := new(big.Int).Mul(p, big.NewInt(3))
		q.Add(q, big.NewInt(2))
		emit("prime", h(q), "10", "", fmt.Sprint(q.ProbablyPrime(10)))
	}
	for _, s := range []string{
		"2047", "3277", "4033", "4681", "8321", "1373653", "25326001", "3215031751",
		"2152302898747", "3474749660383", "341550071728321", "3825123056546413051",
		"318665857834031151167461", "3317044064679887385961981",
		"989", "3239", "5777", "10877", "27971", "29681", "30739", "31631", "39059",
		"72389", "73919", "75077", "100127", "113573", "125249", "137549", "137801",
		"153931", "155819", "161027", "162133", "189419", "218321", "231703",
		"249331", "370229", "429479", "430127", "459191", "473891", "480689",
		"600059", "621781", "632249", "635627",
		"3673744903", "3281593591", "2385076987", "2738053141", "2009621503",
		"1502682721", "255866131", "117987841", "587861", "6368689", "8725753",
		"80579735209", "105919633",
		"2", "3", "5", "7", "11", "13756265695458089029",
		"13496181268022124907", "10953742525620032441", "17908251027575790097",
		"18699199384836356663",
		"98920366548084643601728869055592650835572950932266967461790948584315647",
		"94560208308847015747498523884063394671606671904944666360068158221458669711639",
		"449417999055441493994709297093108513015373787049558499205492347871729927573118262811508386655998299074566974373711472560655026288668094291699357843464363003144674940345912431129144354948751003607115263071543163",
		"230975859993204150666423538988557839555560243929065415434980904258310530753006723857139742334640122533598517597674807096648905501653461687601339782814316124971547968912893214002992086353183070342498989426570593",
		"5521712099665906221540423207019333379125265462121169655563495403888449493493629943498064604536961775110765377745550377067893607246020694972959780839151452457728855382113555867743022746090187341871655890805971735385789993",
		"203956878356401977405765866929034577280193993314348263094772646453283062722701277632936616063144088173312372882677123879538709400158306567338328279154499698366071906766440037074217117805690872792848149112022286332144876183376326512083574821647933992961249917319836219304274280243803104015000563790123",
		"3618502788666131106986593281521497120414687020801267626233049500247285301239",
		"57896044618658097711785492504343953926634992332820282019728792003956564819949",
		"9850501549098619803069760025035903451269934817616361666987073351061430442874302652853566563721228910201656997576599",
		"42307582002575910332922579714097346549017899709713998034217522897561970639123926132812109468141778230245837569601494931472367",
		"6325427930871125737564271130573413447808213095226130620574108289592547700823143543734154025919716924006263232124117211553289271897101021101007391041543003223",
		"10384593717069655257060992658440191",
		"1195068768795265792518361315725116351898245581",
		"8038374574536394912570796143419421081388376882875581458374889175222974273765333652186502336163960045457915042023603208766569966760987284043965408232928738791850869166857328267761771029389697739470167082304286871099974399765441448453411558724506334092790222752962294149842306881685404326457534018329786111298960644845216191652872597534901",
		"18446744073709551557", "18446744073709551615", "18446744073709551617",
		"340282366920938463463374607431768211297", "340282366920938463463374607431768211455",
	} {
		x, _ := new(big.Int).SetString(s, 10)
		for _, n := range []int{0, 1, 5, 20} {
			emit("prime", h(x), fmt.Sprint(n), "", fmt.Sprint(x.ProbablyPrime(n)))
		}
	}

	// Text in every base, and long numbers for the divisor tables.
	for i := 0; i < 200; i++ {
		x := rnd()
		base := 2 + rng.Intn(61)
		if i%4 == 0 {
			base = 10
		}
		emit("text", h(x), fmt.Sprint(base), "", x.Text(base))
	}
	for _, n := range []int{150, 300, 700, 1500} {
		x := random(n, false)
		emit("text", h(x), "10", "", x.Text(10))
		emit("text", h(x), "7", "", x.Text(7))
		emit("setstring", x.Text(10), "10", "", h(x))
	}
	for i := 0; i < 150; i++ {
		x := rnd()
		base := 2 + rng.Intn(61)
		s := x.Text(base)
		emit("setstring", s, fmt.Sprint(base), "", h(x))
		emit("setstring", x.Text(10), "0", "", h(x))
	}
	for _, t := range []struct {
		s    string
		base int
	}{
		{"", 0}, {"", 10}, {"0", 0}, {"-0", 0}, {"+0", 0}, {"0", 10}, {"1", 0},
		{"-1", 0}, {"+1", 0}, {"--1", 0}, {"-+1", 0}, {"1-", 0}, {"a", 0},
		{"0x", 0}, {"0x", 16}, {"0X10", 0}, {"-0x10", 0}, {"0b1011", 0},
		{"0B1011", 0}, {"0o777", 0}, {"0O777", 0}, {"0777", 0}, {"0777", 10},
		{"0_1_2", 0}, {"1_000_000", 0}, {"1__0", 0}, {"_1", 0}, {"1_", 0},
		{"0x_ff", 0}, {"0x_ff", 16}, {"1_000", 10}, {"0b1012", 0}, {"0o8", 0},
		{"12345678901234567890123456789", 0}, {"-12345678901234567890123456789", 10},
		{"zz", 36}, {"ZZ", 36}, {"zZ", 62}, {"Zz", 62}, {"10", 62}, {"z", 35},
		{"101", 2}, {"102", 2}, {" 1", 0}, {"1 ", 0}, {"0x1f.2", 0}, {"1.5", 10},
		{"0xfffffffffffffffffffffffffffffffffffff", 0}, {"0b", 0}, {"0o", 0},
		{"00", 0}, {"-00x10", 0}, {"0x-10", 0}, {"1e3", 10},
	} {
		x, ok := new(big.Int).SetString(t.s, t.base)
		want := "nil"
		if ok {
			want = h(x)
		}
		emit("setstring", t.s, fmt.Sprint(t.base), "", want)
	}

	// Format, with the flags, widths and precisions intconv_test.go uses.
	formats := []string{
		"%d", "%v", "%s", "%b", "%o", "%O", "%x", "%X", "%#b", "%#o", "%#O", "%#x",
		"%#X", "%+d", "% d", "%+ d", "%10d", "%-10d|", "%010d", "%-010d|", "%.20d",
		"%8.5d", "%-8.5d|", "%08.5d", "%.0d", "%.d", "%#10x", "%#-10X|", "%#010x",
		"%+#o", "%q", "%c", "%e", "%U", "%t", "%#v", "%3.0d", "%+.10b", "% 012x",
	}
	vals := []*big.Int{big.NewInt(0), big.NewInt(1), big.NewInt(-1), big.NewInt(1234),
		big.NewInt(-1234), big.NewInt(255), random(2, false), random(3, true)}
	for _, f := range formats {
		for _, x := range vals {
			emit("format", h(x), f, "", fmt.Sprintf(f, x))
		}
	}

	// Scan, through fmt.Sscanf with each verb.
	for _, t := range []struct{ in, verb string }{
		{"1011001", "%b"}, {"-1011001", "%b"}, {"12", "%b"}, {"0b11", "%b"},
		{"017", "%o"}, {"-0o17", "%o"}, {"19", "%o"}, {"12345", "%d"},
		{"-12345", "%d"}, {"+12345", "%d"}, {"0x12", "%d"}, {"ff", "%x"},
		{"-FF", "%X"}, {"0xff", "%x"}, {"0x1F", "%v"}, {"0b101", "%v"},
		{"0o17", "%s"}, {"017", "%v"}, {"1_000", "%v"}, {"   42", "%d"},
		{"42 43", "%d"}, {"", "%d"}, {"-", "%d"}, {"x", "%d"}, {"12", "%q"},
		{"12", "%e"}, {"123456789012345678901234567890", "%v"}, {"12x", "%v"},
		{"99", "%3d"}, {"12345", "%3d"},
	} {
		x := new(big.Int)
		_, err := fmt.Sscanf(t.in, t.verb, x)
		want := h(x)
		if err != nil {
			want = "err:" + err.Error()
		}
		emit("scan", t.in, t.verb, "", want)
	}

	// Float64, around 2**53, at ties, and too big.
	for i := 0; i < 150; i++ {
		x := random(1+rng.Intn(3), rng.Intn(2) == 0)
		switch i % 5 {
		case 1:
			x = new(big.Int).Lsh(big.NewInt(1), uint(50+rng.Intn(20)))
			x.Add(x, big.NewInt(int64(rng.Intn(9)-4)))
		case 2:
			x = new(big.Int).Lsh(big.NewInt(int64(rng.Int63n(1<<53)|1)), uint(1+rng.Intn(60)))
			x.Add(x, new(big.Int).Lsh(big.NewInt(1), uint(rng.Intn(3))))
		case 3:
			x = random(15+rng.Intn(3), rng.Intn(2) == 0)
		case 4:
			x = new(big.Int).Lsh(big.NewInt(int64(rng.Int63n(1<<54))), 970)
			if rng.Intn(2) == 0 {
				x.Neg(x)
			}
		}
		f, acc := x.Float64()
		emit("float64", h(x), "", "", fmt.Sprintf("%016x %s", math.Float64bits(f), acc))
	}

	for i := 0; i < 150; i++ {
		x := random(rng.Intn(3), rng.Intn(2) == 0)
		if i%6 == 0 {
			x = big.NewInt(math.MinInt64)
		}
		if i%6 == 1 {
			x = new(big.Int).SetUint64(math.MaxUint64)
		}
		if i%6 == 2 {
			x = new(big.Int).Neg(new(big.Int).SetUint64(1 << 63))
			x.Sub(x, big.NewInt(int64(rng.Intn(3))))
		}
		emit("int64", h(x), "", "", fmt.Sprintf("%d %d %t %t", x.Int64(), x.Uint64(), x.IsInt64(), x.IsUint64()))
	}

	// Bytes, FillBytes, SetBytes and the gob, text and JSON forms.
	for i := 0; i < 100; i++ {
		x := rnd()
		emit("bytes", h(x), "", "", hex.EncodeToString(x.Bytes()))
		g, _ := x.GobEncode()
		emit("gob", h(x), "", "", hex.EncodeToString(g))
		n := len(x.Bytes()) + rng.Intn(4)
		emit("fillbytes", h(x), fmt.Sprint(n), "", hex.EncodeToString(x.FillBytes(make([]byte, n))))
		b := make([]byte, rng.Intn(40))
		rng.Read(b)
		if len(b) > 0 && i%3 == 0 {
			b[0] = 0
		}
		emit("setbytes", hex.EncodeToString(b), "", "", h(new(big.Int).SetBytes(b)))
		j, _ := x.MarshalJSON()
		emit("json", h(x), "", "", string(j))
		t, _ := x.MarshalText()
		emit("text", h(x), "10", "", string(t))
	}
	for _, s := range []string{"", "0", "-0", "123", "-0x1f", "0b101", "1_000", "12a", " 1", "null", "nul", "1.0"} {
		x := new(big.Int)
		err := x.UnmarshalText([]byte(s))
		want := h(x)
		if err != nil {
			want = "err:" + err.Error()
		}
		emit("unmarshaltext", s, "", "", want)
		x = big.NewInt(77)
		err = x.UnmarshalJSON([]byte(s))
		want = h(x)
		if err != nil {
			want = "err:" + err.Error()
		}
		emit("unmarshaljson", s, "", "", want)
	}
	for _, s := range []string{"", "02", "03", "0201", "03ff", "02000001", "04", "0000", "0101"} {
		b, _ := hex.DecodeString(s)
		x := big.NewInt(5)
		err := x.GobDecode(b)
		want := h(x)
		if err != nil {
			want = "err:" + err.Error()
		}
		emit("gobdecode", s, "", "", want)
	}

	// Binomial and MulRange, from int_test.go and random.
	for _, t := range [][2]int64{
		{0, 0}, {0, 1}, {1, 0}, {1, 1}, {1, 10}, {4, 0}, {4, 1}, {4, 2}, {4, 3},
		{4, 4}, {10, 1}, {10, 9}, {10, 5}, {11, 5}, {11, 6}, {1000, 500},
		{1000, 1000}, {-1, 1}, {5, -1}, {1000000000000, 3}, {100, 97}, {128, 64},
	} {
		emit("binomial", fmt.Sprint(t[0]), fmt.Sprint(t[1]), "", h(new(big.Int).Binomial(t[0], t[1])))
	}
	for _, t := range [][2]int64{
		{0, 0}, {1, 1}, {1, 2}, {1, 3}, {10, 10}, {0, 100}, {0, 1e9},
		{1, 0}, {1, 100}, {-100, -1}, {-5, 5}, {-10, -10}, {-10, -9}, {-10, -8},
		{1, 20}, {1, 21}, {1, 50}, {-50, -1}, {-49, -1}, {100, 150}, {1e9, 1e9 + 30},
		{math.MaxInt64 - 3, math.MaxInt64}, {math.MinInt64, math.MinInt64 + 2},
		{math.MinInt64, math.MinInt64}, {-3, math.MinInt64},
	} {
		emit("mulrange", fmt.Sprint(t[0]), fmt.Sprint(t[1]), "", h(new(big.Int).MulRange(t[0], t[1])))
	}

	// Rand, with math/rand's seeded source.
	for i := 0; i < 60; i++ {
		n := rnd()
		seed := rng.Int63()
		r := rand.New(rand.NewSource(seed))
		emit("rand", h(n), fmt.Sprint(seed), "", h(new(big.Int).Rand(r, n)))
	}
}
GO

cd "$tmp"
go mod init biggen > /dev/null 2>&1
go run . > "$tmp/cases.inc"

{
	cat <<'HDR'
/* What Go's math/big says about a few thousand Int operations, one row each:
 * the operation, up to three operands and the result, as text. Numbers are in
 * base 16 with a sign. Regenerate it with tools/gen-math-big-tests.sh rather
 * than editing it.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

typedef struct BigCase {
    const char *op;
    const char *a;
    const char *b;
    const char *c;
    const char *want;
} BigCase;

/* clang-format off */
static const BigCase big_cases[] = {
HDR
	cat "$tmp/cases.inc"
	printf '};\n/* clang-format on */\n'
} > "$out"
