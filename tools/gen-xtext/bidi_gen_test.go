// Laid over golang.org/x/text/unicode/bidi by tools/gen-xtext-bidi.sh, so that
// it can read the package's unexported tables and its core by name. It writes
// the C tables, a header for them, and the test data for
// tests/xtext_bidi_test.c.
//
// Copyright 2026 The burrow Authors. All rights reserved.
// Use of this source code is governed by a BSD-style licence that can be found
// in the LICENSE file.

package bidi

import (
	"bytes"
	"fmt"
	"hash/fnv"
	"io"
	"math/rand/v2"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// cstr is s as a C string literal, cut into pieces of at most 72 bytes of
// source. Octal escapes are always three digits, so a digit after one cannot
// be read as part of it.
func cstr(s string) string {
	if s == "" {
		return `""`
	}
	var out, cur strings.Builder
	flush := func() {
		if cur.Len() > 0 {
			if out.Len() > 0 {
				out.WriteString("\n        ")
			}
			out.WriteString(`"` + cur.String() + `"`)
			cur.Reset()
		}
	}
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == '"' || c == '\\':
			cur.WriteByte('\\')
			cur.WriteByte(c)
		case c == '?':
			cur.WriteString(`\077`)
		case c >= 0x20 && c < 0x7f:
			cur.WriteByte(c)
		default:
			fmt.Fprintf(&cur, "\\%03o", c)
		}
		if cur.Len() >= 72 {
			flush()
		}
	}
	flush()
	return out.String()
}

func clit(s string) string {
	return fmt.Sprintf("%s, %d", cstr(s), len(s))
}

func cints[T int | int8 | int32 | uint8 | uint16 | Class | bracketType](w io.Writer, ctype, name string, v []T, perLine int, hex bool) {
	fmt.Fprintf(w, "%s %s[%d] = {\n", ctype, name, max(len(v), 1))
	if len(v) == 0 {
		fmt.Fprintln(w, "    0,")
	}
	for i, x := range v {
		if i%perLine == 0 {
			fmt.Fprint(w, "    ")
		}
		if hex {
			fmt.Fprintf(w, "0x%x,", x)
		} else {
			fmt.Fprintf(w, "%d,", x)
		}
		if i%perLine == perLine-1 || i == len(v)-1 {
			fmt.Fprintln(w)
		} else {
			fmt.Fprint(w, " ")
		}
	}
	fmt.Fprintln(w, "};")
	fmt.Fprintln(w)
}

func write(t *testing.T, path string, b []byte) {
	if err := os.WriteFile(path, b, 0o644); err != nil {
		t.Fatal(err)
	}
}

func writeTables(t *testing.T, srcDir, banner string) {
	var h bytes.Buffer
	fmt.Fprintf(&h, banner, "tables"+UnicodeVersion+".go")
	fmt.Fprintf(&h, "#ifndef BURROW_SRC_XTEXT_BIDI_TABLES_H\n#define BURROW_SRC_XTEXT_BIDI_TABLES_H\n\n")
	fmt.Fprintf(&h, "#include \"bidi.h\"\n\n#include <stdint.h>\n\n")
	fmt.Fprintf(&h, "/* clang-format off */\n\n")
	fmt.Fprintf(&h, "#define BIDI_UNICODE_VERSION %q\n\n", UnicodeVersion)
	fmt.Fprintf(&h, "extern const int32_t burrow__bidi_xor_masks[%d];\n", len(xorMasks))
	fmt.Fprintf(&h, "extern const uint8_t burrow__bidi_values[%d];\n", len(bidiValues))
	fmt.Fprintf(&h, "extern const uint16_t burrow__bidi_index[%d];\n\n", len(bidiIndex))
	fmt.Fprintf(&h, "/* clang-format on */\n\n#endif /* BURROW_SRC_XTEXT_BIDI_TABLES_H */\n")

	var c bytes.Buffer
	fmt.Fprintf(&c, banner, "tables"+UnicodeVersion+".go")
	fmt.Fprintf(&c, "#include \"bidi.h\"\n#include \"bidi_tables.h\"\n\n#include <stdint.h>\n\n")
	fmt.Fprintf(&c, "/* clang-format off */\n\n")
	cints(&c, "const int32_t", "burrow__bidi_xor_masks", xorMasks, 8, false)
	cints(&c, "const uint8_t", "burrow__bidi_values", bidiValues[:], 16, true)
	cints(&c, "const uint16_t", "burrow__bidi_index", bidiIndex[:], 12, true)
	fmt.Fprintf(&c, "/* clang-format on */\n")

	write(t, filepath.Join(srcDir, "bidi_tables.h"), h.Bytes())
	write(t, filepath.Join(srcDir, "bidi_tables.c"), c.Bytes())
}

// ---------------------------------------------------------------- records

type record struct{ buf bytes.Buffer }

func (r *record) s(tag string, s []byte) {
	fmt.Fprintf(&r.buf, "%s%d:", tag, len(s))
	r.buf.Write(s)
}

func (r *record) n(tag string, n int) { fmt.Fprintf(&r.buf, "%s%d;", tag, n) }

func (r *record) e(tag string, err error) {
	if err == nil {
		fmt.Fprintf(&r.buf, "%s-;", tag)
	} else {
		fmt.Fprintf(&r.buf, "%s%s;", tag, err.Error())
	}
}

func (r *record) bool(tag string, b bool) {
	if b {
		r.n(tag, 1)
	} else {
		r.n(tag, 0)
	}
}

func hash64(b []byte) uint64 {
	h := fnv.New64a()
	h.Write(b)
	return h.Sum64()
}

// catch runs fn and records the panic, if it panics.
func catch(r *record, fn func()) {
	defer func() {
		if p := recover(); p != nil {
			fmt.Fprintf(&r.buf, "PANIC%v;", p)
		}
	}()
	fn()
}

// ---------------------------------------------------------------- the core

type coreCase struct {
	types      []Class
	pairTypes  []bracketType
	pairValues []rune
	nilValues  bool
	level      level
	breaks     []int
}

func describeCore(c coreCase) []byte {
	var r record
	catch(&r, func() {
		pv := c.pairValues
		if c.nilValues {
			pv = nil
		}
		p, err := newParagraph(c.types, c.pairTypes, pv, c.level)
		r.e("E", err)
		if err != nil {
			return
		}
		r.n("L", int(p.embeddingLevel))
		r.n("N", p.Len())
		catch(&r, func() {
			for _, l := range p.getLevels(c.breaks) {
				r.n("", int(l))
			}
		})
		catch(&r, func() {
			for _, o := range p.getReordering(c.breaks) {
				r.n("O", o)
			}
		})
	})
	return r.buf.Bytes()
}

// The classes the random cases are drawn from, weighted toward the ones the
// algorithm does the most with.
var coreClasses = []Class{
	L, L, R, R, AL, EN, EN, ES, ET, AN, AN, CS, S, WS, WS, ON, ON, ON, BN, NSM, NSM,
	LRO, RLO, LRE, RLE, PDF, PDF, LRI, RLI, FSI, PDI, PDI,
}

func coreCases() []coreCase {
	rng := rand.New(rand.NewPCG(2143, 59))
	var out []coreCase
	for i := 0; i < 1500; i++ {
		n := 1 + rng.IntN(16)
		if i%10 == 9 {
			n = 20 + rng.IntN(60)
		}
		c := coreCase{level: level(rng.IntN(3) - 1)}
		for j := 0; j < n; j++ {
			t := coreClasses[rng.IntN(len(coreClasses))]
			pt, pv := bpNone, rune(0)
			if t == ON && rng.IntN(2) == 0 {
				pt = bracketType(1 + rng.IntN(2))
				pv = rune("([{"[rng.IntN(3)])
			}
			c.types = append(c.types, t)
			c.pairTypes = append(c.pairTypes, pt)
			c.pairValues = append(c.pairValues, pv)
		}
		if i%5 == 4 {
			c.types[n-1] = B
		}
		// Deep nesting, past the 125 levels and the 63 open brackets.
		if i%50 == 49 {
			for j := 0; j < 140; j++ {
				t := []Class{RLE, LRE, RLI, LRI, ON}[i/50%5]
				c.types = append([]Class{t}, c.types...)
				c.pairTypes = append([]bracketType{bpOpen}, c.pairTypes...)
				c.pairValues = append([]rune{'('}, c.pairValues...)
			}
			n = len(c.types)
		}
		c.breaks = []int{n}
		if n > 3 && rng.IntN(3) == 0 {
			k := 1 + rng.IntN(n-2)
			c.breaks = []int{k, n}
		}
		out = append(out, c)
	}
	// The errors newParagraph returns, and the panics past them.
	out = append(out,
		coreCase{types: nil, pairTypes: []bracketType{0}, pairValues: []rune{0}, breaks: []int{1}},
		coreCase{types: []Class{L, B, L}, pairTypes: []bracketType{0, 0, 0}, pairValues: []rune{0, 0, 0}, breaks: []int{3}},
		coreCase{types: []Class{L}, pairTypes: nil, pairValues: []rune{0}, breaks: []int{1}},
		coreCase{types: []Class{L, ON}, pairTypes: []bracketType{0, 3}, pairValues: []rune{0, 0}, breaks: []int{2}},
		coreCase{types: []Class{L}, pairTypes: []bracketType{0}, nilValues: true, breaks: []int{1}},
		coreCase{types: []Class{L, L}, pairTypes: []bracketType{0, 0}, pairValues: []rune{0}, breaks: []int{2}},
		coreCase{types: []Class{L}, pairTypes: []bracketType{0}, pairValues: []rune{0}, level: 2, breaks: []int{1}},
		coreCase{types: []Class{L}, pairTypes: []bracketType{0}, pairValues: []rune{0}, level: -5, breaks: []int{1}},
		coreCase{types: []Class{L, ON, ON}, pairTypes: []bracketType{0, 1}, pairValues: []rune{0, '('}, breaks: []int{3}},
		coreCase{types: []Class{L, Control, R}, pairTypes: []bracketType{0, 0, 0}, pairValues: []rune{0, 0, 0}, breaks: []int{3}},
		coreCase{types: []Class{R, numClass, R}, pairTypes: []bracketType{0, 0, 0}, pairValues: []rune{0, 0, 0}, breaks: []int{3}},
		coreCase{types: []Class{L, WS, R}, pairTypes: []bracketType{0, 0, 0}, pairValues: []rune{0, 0, 0}, breaks: []int{4}},
		coreCase{types: []Class{L, WS, R}, pairTypes: []bracketType{0, 0, 0}, pairValues: []rune{0, 0, 0}, breaks: []int{2}},
		coreCase{types: []Class{L, WS, R}, pairTypes: []bracketType{0, 0, 0}, pairValues: []rune{0, 0, 0}, breaks: []int{2, 1, 3}},
		coreCase{types: []Class{R, WS, R}, pairTypes: []bracketType{0, 0, 0}, pairValues: []rune{0, 0, 0}, breaks: nil},
		coreCase{types: []Class{FSI, R, L}, pairTypes: []bracketType{0, 0, 0}, pairValues: []rune{0, 0, 0}, level: -1, breaks: []int{3}},
	)
	return out
}

func writeCore(t *testing.T, w io.Writer) {
	cs := coreCases()
	var types []Class
	var pts []bracketType
	var pvs []rune
	var brks []int
	type off struct{ t, nt, p, np, v, nv, b, nb int }
	var offs []off
	for _, c := range cs {
		o := off{len(types), len(c.types), len(pts), len(c.pairTypes), len(pvs), len(c.pairValues), len(brks), len(c.breaks)}
		types = append(types, c.types...)
		pts = append(pts, c.pairTypes...)
		pvs = append(pvs, c.pairValues...)
		brks = append(brks, c.breaks...)
		offs = append(offs, o)
	}
	cints(w, "static const uint8_t", "bidi_core_types", types, 24, false)
	cints(w, "static const uint8_t", "bidi_core_pair_types", pts, 24, false)
	cints(w, "static const int32_t", "bidi_core_pair_values", pvs, 16, false)
	cints(w, "static const Int", "bidi_core_breaks", brks, 16, false)
	fmt.Fprintf(w, "/* Offsets and counts into the arrays above: types, pair types, pair\n")
	fmt.Fprintf(w, " * values and line breaks, then whether the pair values are nil, the level\n")
	fmt.Fprintf(w, " * and the hash of what Go says about the case. See describeCore. */\n")
	fmt.Fprintf(w, "static const BidiCoreCase bidi_core_cases[] = {\n")
	for i, c := range cs {
		o := offs[i]
		fmt.Fprintf(w, "    {%d, %d, %d, %d, %d, %d, %d, %d, %v, %d, 0x%016xU},\n",
			o.t, o.nt, o.p, o.np, o.v, o.nv, o.b, o.nb, c.nilValues, c.level, hash64(describeCore(c)))
	}
	fmt.Fprintf(w, "};\n\n")
}

// ---------------------------------------------------------------- the API

func describeRun(r *record, run Run) {
	r.s("", []byte(run.String()))
	r.s("", run.Bytes())
	s, e := run.Pos()
	r.n("", s)
	r.n("", e)
	r.n("", int(run.Direction()))
}

func describeOrdering(r *record, o Ordering) {
	r.n("R", o.NumRuns())
	catch(r, func() { r.n("OD", int(o.Direction())) })
	for i := 0; i < o.NumRuns(); i++ {
		describeRun(r, o.Run(i))
	}
}

func describeOrder(r *record, p *Paragraph) {
	catch(r, func() {
		o, err := p.Order()
		r.e("O", err)
		describeOrdering(r, o)
	})
	catch(r, func() { r.n("D", int(p.Direction())) })
	catch(r, func() { r.bool("LTR", p.IsLeftToRight()) })
	for _, pos := range []int{-1, 0, 1, 2, len(p.runes) - 1, len(p.runes), len(p.runes) + 1} {
		catch(r, func() { describeRun(r, p.RunAt(pos)) })
	}
}

var optionSets = [][]Option{
	nil,
	{DefaultDirection(LeftToRight)},
	{DefaultDirection(RightToLeft)},
	{DefaultDirection(RightToLeft), DefaultDirection(LeftToRight)},
	{DefaultDirection(LeftToRight), DefaultDirection(RightToLeft)},
}

func describe(s string) []byte {
	var r record
	b := []byte(s)
	for _, opts := range optionSets {
		var p Paragraph
		catch(&r, func() {
			n, err := p.SetString(s, opts...)
			r.n("N", n)
			r.e("", err)
		})
		describeOrder(&r, &p)
		nt := len(p.types)
		lines := [][2]int{
			{0, nt}, {0, 1}, {1, nt}, {nt / 2, nt}, {0, nt + 1}, {0, nt + 5}, {nt, nt}, {2, 1},
			{0, cap(p.types)}, {0, cap(p.types) + 1}, {1, len(p.runes) + 1}, {0, len(p.runes)},
		}
		for _, l := range lines {
			catch(&r, func() {
				o, err := p.Line(l[0], l[1])
				r.e("L", err)
				describeOrdering(&r, o)
			})
		}
	}

	// SetBytes, and the options carrying over from one Order to the next.
	var p Paragraph
	catch(&r, func() {
		n, err := p.SetBytes(b)
		r.n("NB", n)
		r.e("", err)
	})
	describeOrder(&r, &p)
	catch(&r, func() { p.SetString(s, DefaultDirection(RightToLeft)) })
	describeOrder(&r, &p)
	catch(&r, func() { p.SetString(s) })
	describeOrder(&r, &p)
	catch(&r, func() { p.SetString("") })
	describeOrder(&r, &p)

	catch(&r, func() { r.s("AR", AppendReverse(nil, b)) })
	catch(&r, func() { r.s("AR", AppendReverse([]byte("Hëllo"), b)) })
	catch(&r, func() { r.s("RS", []byte(ReverseString(s))) })

	for i := 0; i <= len(b); i++ {
		catch(&r, func() {
			p, sz := Lookup(b[i:])
			r.n("K", int(p.Class()))
			r.n("", sz)
			r.bool("", p.IsBracket())
			r.bool("", p.IsOpeningBracket())
		})
		catch(&r, func() {
			p, sz := LookupString(s[i:])
			r.n("KS", int(p.Class()))
			r.n("", sz)
		})
	}
	return r.buf.Bytes()
}

// The runes the random strings are made from: strong runes of each direction,
// numbers and their separators, neutrals, brackets, marks, the explicit
// formatting characters, paragraph separators and bytes that are not UTF-8.
var palette = []string{
	"a", "b", "Z", "\u00f6", "\u05d0", "\u05d1", "\u0627", "\u0628",
	"1", "2", "\u0661", "\u06f1", "+", "-", ".", ",",
	":", "$", "%", " ", "\u00a0", "\u2003", "!", "?",
	"(", ")", "[", "]", "{", "}", "\u2329", "\u232a",
	"\u0300", "\u0301", "\u064b", "\u00ad", "\u200b", "\t", "\u202a", "\u202b",
	"\u202c", "\u202d", "\u202e", "\u2066", "\u2067", "\u2068", "\u2069", "\n",
	"\u2029", "\r", "\U00020000", "\U0001e900", "\xff", "\x80", "\xe2\x80", "\xf0\x9f",
}

func randomStrings() []string {
	rng := rand.New(rand.NewPCG(2143, 17))
	var out []string
	for i := 0; i < 700; i++ {
		var b strings.Builder
		n := 1 + rng.IntN(20)
		if i%10 == 9 {
			n = 30 + rng.IntN(60)
		}
		for j := 0; j < n; j++ {
			// Leave out the separators most of the time, so that most strings
			// are one paragraph.
			for {
				x := palette[rng.IntN(len(palette))]
				if (x == "\n" || x == "\u2029" || x == "\r") && rng.IntN(4) != 0 {
					continue
				}
				b.WriteString(x)
				break
			}
		}
		out = append(out, b.String())
	}
	return out
}

func corpus() []string {
	out := []string{
		"", "Hellö", "ا", "Uا", "+", "Hello\nworld", "(Hello)", "nice (wörld)",
		"\U00020000", "ö", "ॡ",
		"العاشر ليونيكود (Unicode Conference)، الذي سيعقد في 10-12 آذار 1997 مبدينة",
		"The names of these states in Arabic are \u2067مصر\u2069, \u2067البحرين\u2069 and \u2067الكويت\u2069 respectively.",
		"The names of these states in Arabic are مصر, البحرين and الكويت respectively.",
		"א(b)c", "a(א)ב", "א [a] (ב)", "a〈b〉", "ا 1.2 ١٢",
		"\xff", "a\xffb", "א\xe2\x80", "\n", " abc", "abc א",
		strings.Repeat("(", 70) + "א" + strings.Repeat(")", 70),
		strings.Repeat("\u202b", 130) + "a\u05d0" + strings.Repeat("\u202c", 130),
		strings.Repeat("\u2067", 130) + "a\u05d0" + strings.Repeat("\u2069", 130),
	}
	return append(out, randomStrings()...)
}

func writeCorpus(t *testing.T, w io.Writer) {
	cs := corpus()
	fmt.Fprintf(w, "/* The strings the C test describes, with the hash of what Go says about\n")
	fmt.Fprintf(w, " * each. See describe in the generator. */\n")
	fmt.Fprintf(w, "static const BidiCorpus bidi_corpus[] = {\n")
	for _, s := range cs {
		fmt.Fprintf(w, "    {%s,\n     0x%016xU},\n", clit(s), hash64(describe(s)))
	}
	fmt.Fprintf(w, "};\n\n")
}

// ---------------------------------------------------------------- the sweep

func sweepRecord(r rune) []byte {
	var rec record
	p, sz := LookupRune(r)
	rec.n("P", int(p.Class()))
	rec.n("", sz)
	rec.bool("", p.IsBracket())
	rec.bool("", p.IsOpeningBracket())
	if p.IsBracket() {
		rec.n("", int(p.reverseBracket(r)))
	}
	return rec.buf.Bytes()
}

func writeSweep(t *testing.T, w io.Writer) {
	fmt.Fprintf(w, "/* One hash per block of 0x1000 runes, of what LookupRune says about each\n")
	fmt.Fprintf(w, " * rune in it, surrogates included. See sweepRecord. */\n")
	fmt.Fprintf(w, "static const uint64_t bidi_sweep_hash[0x110] = {\n")
	for blk := rune(0); blk < 0x110; blk++ {
		h := fnv.New64a()
		for r := blk << 12; r < (blk+1)<<12; r++ {
			h.Write(sweepRecord(r))
		}
		if blk%3 == 0 {
			fmt.Fprint(w, "    ")
		}
		fmt.Fprintf(w, "0x%016xU,", h.Sum64())
		if blk%3 == 2 || blk == 0x10f {
			fmt.Fprintln(w)
		} else {
			fmt.Fprint(w, " ")
		}
	}
	fmt.Fprintf(w, "};\n")
}

func TestZZGen(t *testing.T) {
	srcDir := os.Getenv("BURROW_XTEXT_SRC")
	testsDir := os.Getenv("BURROW_XTEXT_TESTS")
	banner := os.Getenv("BURROW_XTEXT_BANNER")
	if srcDir == "" || testsDir == "" || banner == "" {
		t.Skip("run by tools/gen-xtext-bidi.sh")
	}
	if dbg := os.Getenv("BURROW_XTEXT_DESCRIBE"); dbg != "" {
		// For chasing a mismatch: what Go says about corpus entry n, or with
		// a c in front, core case n.
		var n int
		if strings.HasPrefix(dbg, "c") {
			fmt.Sscanf(dbg[1:], "%d", &n)
			fmt.Printf("%q\n", describeCore(coreCases()[n]))
			return
		}
		fmt.Sscanf(dbg, "%d", &n)
		fmt.Printf("%q\n", describe(corpus()[n]))
		return
	}
	writeTables(t, srcDir, banner)

	var b bytes.Buffer
	b.WriteString(strings.TrimRight(os.Getenv("BURROW_XTEXT_TEST_BANNER"), "\n") + "\n\n")
	b.WriteString("#ifndef BURROW_TESTS_XTEXT_BIDI_TEST_GEN_H\n#define BURROW_TESTS_XTEXT_BIDI_TEST_GEN_H\n\n")
	b.WriteString("/* clang-format off */\n\n")
	writeCore(t, &b)
	writeCorpus(t, &b)
	writeSweep(t, &b)
	b.WriteString("\n/* clang-format on */\n\n#endif /* BURROW_TESTS_XTEXT_BIDI_TEST_GEN_H */\n")
	write(t, filepath.Join(testsDir, "xtext_bidi_test_gen.h"), b.Bytes())
}
