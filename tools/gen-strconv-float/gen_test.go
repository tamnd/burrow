// Writes tests/strconv_float_gen.h from Go's own strconv. It runs as a test
// of a copy of internal/strconv, since that is where the tables live, and
// tools/gen-strconv-float.sh sets that copy up.

package strconv_test

import (
	"fmt"
	"math"
	"math/rand"
	"os"
	. "scgen/sc"
	"strings"
	"testing"
)

func cstr(s string) string {
	var b strings.Builder
	b.WriteByte('"')
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == '"' || c == '\\':
			b.WriteByte('\\')
			b.WriteByte(c)
		case c >= 0x20 && c < 0x7f:
			b.WriteByte(c)
		default:
			fmt.Fprintf(&b, "\\x%02x\" \"", c)
		}
	}
	b.WriteByte('"')
	return b.String()
}

func cerr(e error) string {
	switch e {
	case nil:
		return "E_NONE"
	case ErrSyntax:
		return "E_SYNTAX"
	case ErrRange:
		return "E_RANGE"
	}
	panic(e)
}

func cfmt(b byte) string {
	return fmt.Sprintf("'%c'", b)
}

const header = `/* What Go's strconv says for the inputs in tools/gen-strconv-float: the tables
 * from atof_test.go, ftoa_test.go and atoc_test.go, and a few thousand values
 * drawn at random with a fixed seed. Regenerate it with
 * tools/gen-strconv-float.sh rather than editing it.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

`

func TestGenFloat(t *testing.T) {
	w, err := os.Create(os.Getenv("OUT"))
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()
	fmt.Fprint(w, header)
	r := rand.New(rand.NewSource(2143))

	// Parsing: Go's tables, then random shortest forms, then long inputs
	// that the fast path cannot settle.
	var ins []string
	seen := map[string]bool{}
	add := func(s string) {
		if !seen[s] {
			seen[s] = true
			ins = append(ins, s)
		}
	}
	for _, c := range atoftests {
		add(c.in)
	}
	for _, c := range atof32tests {
		add(c.in)
	}
	add("4.87402195346389e+27")
	add("4.8740219534638903e+27")
	for i := 0; i < 300; i++ {
		x := math.Float64frombits(r.Uint64())
		add(FormatFloat(x, 'g', -1, 64))
		y := math.Float32frombits(r.Uint32())
		add(FormatFloat(float64(y), 'g', -1, 32))
	}
	for i := 0; i < 200; i++ {
		// Exactly halfway between two floats, then a digit either side.
		x := math.Ldexp(1+r.Float64(), r.Intn(140)-70)
		up := math.Nextafter(x, math.Inf(1))
		if math.IsInf(up, 0) {
			continue
		}
		lo := exactDecimal(x)
		hi := exactDecimal(up)
		m := midpoint(lo, hi)
		add(m)
		add(m + "1")
		add(strings.TrimRight(m, "0") + "e0")
	}
	for i := 0; i < 100; i++ {
		var b strings.Builder
		n := 20 + r.Intn(60)
		b.WriteByte(byte('1' + r.Intn(9)))
		for j := 0; j < n; j++ {
			b.WriteByte(byte('0' + r.Intn(10)))
		}
		fmt.Fprintf(&b, "e%d", r.Intn(700)-350)
		add(b.String())
	}
	// C99 only promises string literals up to 4095 bytes, so the few longer
	// inputs are written as a prefix, a run of one byte and a suffix.
	var long []string
	fmt.Fprintln(w, "static const AtofCase atof_tests[] = {")
	for _, s := range ins {
		if len(s) > 4000 {
			long = append(long, s)
			continue
		}
		fmt.Fprintf(w, "    {BURROW_S_INIT(%s), %s},\n", cstr(s), atofWant(s))
	}
	fmt.Fprintln(w, "};")
	fmt.Fprintln(w, "\nstatic const LongAtofCase long_atof_tests[] = {")
	for _, s := range long {
		i, j := longestRun(s)
		fmt.Fprintf(w, "    {BURROW_S_INIT(%s), '%c', %d, BURROW_S_INIT(%s), {BURROW_S_INIT(\"\"), %s}},\n",
			cstr(s[:i]), s[i], j-i, cstr(s[j:]), atofWant(s))
	}
	fmt.Fprintln(w, "};")

	// Formatting: Go's table at both sizes, then random values in every
	// format with random precisions.
	fmt.Fprintln(w, "\nstatic const FtoaCase ftoa_tests[] = {")
	emit := func(f float64, c byte, prec, size int) {
		fmt.Fprintf(w, "    {UINT64_C(0x%016x), %s, %d, %d, BURROW_S_INIT(%s)},\n",
			math.Float64bits(f), cfmt(c), prec, size, cstr(FormatFloat(f, c, prec, size)))
	}
	for _, c := range ftoatests {
		emit(c.f, c.fmt, c.prec, 64)
		if c.fmt != 'b' || float64(float32(c.f)) == c.f {
			emit(c.f, c.fmt, c.prec, 32)
		}
	}
	fmts := []byte("eEfgGxXb")
	for i := 0; i < 250; i++ {
		x := math.Float64frombits(r.Uint64())
		y := float64(math.Float32frombits(r.Uint32()))
		for _, v := range []struct {
			f    float64
			size int
		}{{x, 64}, {y, 32}} {
			emit(v.f, 'g', -1, v.size)
			c := fmts[r.Intn(len(fmts))]
			prec := r.Intn(20)
			if r.Intn(4) == 0 {
				prec = 18 + r.Intn(40)
			}
			emit(v.f, c, prec, v.size)
			emit(v.f, fmts[r.Intn(len(fmts))], -1, v.size)
		}
	}
	for i := 0; i < 40; i++ {
		// %f of big and small numbers, which is all digits or all zeros.
		x := math.Ldexp(1+r.Float64(), r.Intn(2000)-1000)
		emit(x, 'f', r.Intn(30), 64)
		emit(x, 'f', 300+r.Intn(800), 64)
	}
	fmt.Fprintln(w, "};")

	fmt.Fprintln(w, "\nstatic const AtocCase atoc_tests[] = {")
	for _, c := range atocTests {
		for _, size := range []int{128, 64} {
			z, err := ParseComplex(c.in, size)
			fmt.Fprintf(w, "    {BURROW_S_INIT(%s), %d, UINT64_C(0x%016x), UINT64_C(0x%016x), %s},\n",
				cstr(c.in), size, math.Float64bits(real(z)), math.Float64bits(imag(z)), cerr(err))
		}
	}
	fmt.Fprintln(w, "};")
}

func atofWant(s string) string {
	f64, e64 := ParseFloat(s, 64)
	f32, e32 := ParseFloat(s, 32)
	return fmt.Sprintf("UINT64_C(0x%016x), %s, UINT32_C(0x%08x), %s",
		math.Float64bits(f64), cerr(e64), math.Float32bits(float32(f32)), cerr(e32))
}

// longestRun gives the bounds of the longest run of one byte in s.
func longestRun(s string) (int, int) {
	bi, bj := 0, 0
	for i := 0; i < len(s); {
		j := i
		for j < len(s) && s[j] == s[i] {
			j++
		}
		if j-i > bj-bi {
			bi, bj = i, j
		}
		i = j
	}
	return bi, bj
}

// exactDecimal gives the exact decimal expansion of a positive float64
// as digits and an exponent, in the form d.ddddde±n.
func exactDecimal(x float64) string {
	return FormatFloat(x, 'e', 1100, 64)
}

// midpoint gives the decimal halfway between two exact %e expansions
// by averaging them as big decimals.
func midpoint(a, b string) string {
	da, ea := splitE(a)
	db, eb := splitE(b)
	// Align on the smaller exponent.
	for ea > eb {
		da += "0"
		ea--
	}
	for eb > ea {
		db += "0"
		eb--
	}
	for len(da) < len(db) {
		da = "0" + da
	}
	for len(db) < len(da) {
		db = "0" + db
	}
	// sum
	sum := make([]byte, len(da)+1)
	carry := 0
	for i := len(da) - 1; i >= 0; i-- {
		s := int(da[i]-'0') + int(db[i]-'0') + carry
		sum[i+1] = byte('0' + s%10)
		carry = s / 10
	}
	sum[0] = byte('0' + carry)
	// halve
	half := make([]byte, len(sum)+1)
	rem := 0
	for i := 0; i < len(sum); i++ {
		v := rem*10 + int(sum[i]-'0')
		half[i] = byte('0' + v/2)
		rem = v % 2
	}
	half[len(sum)] = byte('0' + rem*5)
	digits := strings.TrimLeft(string(half), "0")
	return digits + fmt.Sprintf("e%d", ea-1)
}

// splitE turns d.ddde±n into the digit string and the exponent of its
// last digit.
func splitE(s string) (string, int) {
	i := strings.IndexByte(s, 'e')
	var e int
	fmt.Sscanf(s[i+1:], "%d", &e)
	m := strings.Replace(s[:i], ".", "", 1)
	full := len(m)
	m = strings.TrimRight(m, "0")
	return m, e - (full - 1) + (full - len(m))
}
