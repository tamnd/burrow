// Laid over golang.org/x/text/secure/bidirule by tools/gen-xtext-bidi.sh, so
// that it can read the package's test table by name. It writes the test data
// for tests/xtext_bidirule_test.c: Go's own cases, and what the package says
// about a corpus of labels and about every rune, as hashes.
//
// Copyright 2026 The burrow Authors. All rights reserved.
// Use of this source code is governed by a BSD-style licence that can be found
// in the LICENSE file.

package bidirule

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

	"golang.org/x/text/transform"
)

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

func errCode(err error) int {
	switch err {
	case nil:
		return 0
	case ErrInvalid:
		return 1
	case transform.ErrShortDst:
		return 2
	case transform.ErrShortSrc:
		return 3
	}
	panic(err)
}

type record struct{ buf bytes.Buffer }

func (r *record) n(tag string, n int) { fmt.Fprintf(&r.buf, "%s%d;", tag, n) }

func (r *record) s(tag string, s []byte) {
	fmt.Fprintf(&r.buf, "%s%d:", tag, len(s))
	r.buf.Write(s)
}

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

var dstSizes = []int{0, 1, 2, 3, 5, 8, 64}

func describe(s string) []byte {
	var r record
	b := []byte(s)
	r.n("D", int(Direction(b)))
	r.n("DS", int(DirectionString(s)))
	r.bool("V", Valid(b))
	r.bool("VS", ValidString(s))

	for _, eof := range []bool{false, true} {
		t := New()
		n, err := t.Span(b, eof)
		r.n("SP", n)
		r.e("", err)
		// Again, without a reset, which carries the state over.
		n, err = t.Span(b, eof)
		r.n("SP2", n)
		r.e("", err)
	}

	// Split in two, the way Go's TestSpan and TestTransform do it.
	for i := 0; i <= len(b); i++ {
		t := New()
		n, err := t.Span(b[:i], i == len(b))
		r.n("S", n)
		r.e("", err)
		n2, err := t.Span(b[n:], true)
		r.n("", n2)
		r.e("", err)
	}

	for _, sz := range dstSizes {
		for _, eof := range []bool{false, true} {
			t := New()
			dst := make([]byte, sz)
			nDst, nSrc, err := t.Transform(dst, b, eof)
			r.n("T", nDst)
			r.n("", nSrc)
			r.e("", err)
			r.s("", dst[:nDst])
			t.Reset()
			nDst, nSrc, err = t.Transform(dst, b, eof)
			r.n("TR", nDst)
			r.n("", nSrc)
			r.e("", err)
		}
	}
	return r.buf.Bytes()
}

// One rune of each class and then some, and bytes that are not UTF-8.
var palette = []string{
	"A", "b", "\u00e9", "\u05d0", "\u05e2", "\u062f", "\u0628", "1",
	"\u06f1", "+", "-", "$", "%", "\u0660", "\u0669", ",",
	".", ":", "\u0300", "\u064b", "\u200d", "\u00ad", " ", "\t",
	"\u00a0", "@", "(", "\u202a", "\u2067", "\U00010900", "\U0001d7ce", "\U00020000",
	"\xff", "\x80", "\xd7", "\xe2\x80",
}

func randomStrings() []string {
	rng := rand.New(rand.NewPCG(2143, 23))
	var out []string
	for i := 0; i < 1500; i++ {
		var b strings.Builder
		n := 1 + rng.IntN(8)
		if i%10 == 9 {
			n = 10 + rng.IntN(20)
		}
		for j := 0; j < n; j++ {
			b.WriteString(palette[rng.IntN(len(palette))])
		}
		out = append(out, b.String())
	}
	return out
}

func corpus() []string {
	seen := map[string]bool{}
	var out []string
	add := func(s string) {
		if !seen[s] {
			seen[s] = true
			out = append(out, s)
		}
	}
	add("")
	for _, cases := range testCases {
		for _, tc := range cases {
			add(tc.in)
		}
	}
	for _, s := range randomStrings() {
		add(s)
	}
	return out
}

// The labels the sweep checks each rune in: alone, after an L, after an R,
// before a European number, and between an R and an Arabic number.
func sweepStrings(r rune) []string {
	s := string(r)
	return []string{s, "a" + s, "\u05d0" + s, s + "1", "\u05d0" + s + "\u0660"}
}

func sweepRecord(r rune) []byte {
	var rec record
	for _, s := range sweepStrings(r) {
		rec.n("D", int(DirectionString(s)))
		rec.bool("", ValidString(s))
	}
	return rec.buf.Bytes()
}

func writeTestData(t *testing.T, w io.Writer) {
	fmt.Fprintf(w, "/* testCases from bidirule_test.go, rule by rule, with n filled in the way\n")
	fmt.Fprintf(w, " * its init does. The errors are 0 for none, then ErrInvalid, ErrShortDst\n")
	fmt.Fprintf(w, " * and ErrShortSrc. */\n")
	fmt.Fprintf(w, "static const BidiruleTest bidirule_tests[] = {\n")
	for rule, cases := range testCases {
		for _, tc := range cases {
			fmt.Fprintf(w, "    {%d, %s,\n     %d, %d, %d, %d, %d, %d, %d},\n", rule, clit(tc.in),
				tc.dir, tc.n, errCode(tc.err), tc.pSrc, tc.szDst, tc.nSrc, errCode(tc.err0))
		}
	}
	fmt.Fprintf(w, "};\n\n")

	cs := corpus()
	fmt.Fprintf(w, "/* The labels the C test describes, with the hash of what Go says about\n")
	fmt.Fprintf(w, " * each. See describe in the generator. */\n")
	fmt.Fprintf(w, "static const BidiruleCorpus bidirule_corpus[] = {\n")
	for _, s := range cs {
		fmt.Fprintf(w, "    {%s, 0x%016xU},\n", clit(s), hash64(describe(s)))
	}
	fmt.Fprintf(w, "};\n\n")

	fmt.Fprintf(w, "/* One hash per block of 0x1000 runes, surrogates left out. See\n")
	fmt.Fprintf(w, " * sweepRecord. */\n")
	fmt.Fprintf(w, "static const uint64_t bidirule_sweep_hash[0x110] = {\n")
	for blk := rune(0); blk < 0x110; blk++ {
		h := fnv.New64a()
		for r := blk << 12; r < (blk+1)<<12; r++ {
			if r >= 0xd800 && r < 0xe000 {
				continue
			}
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
	testsDir := os.Getenv("BURROW_XTEXT_TESTS")
	if testsDir == "" {
		t.Skip("run by tools/gen-xtext-bidi.sh")
	}
	if dbg := os.Getenv("BURROW_XTEXT_DESCRIBE"); dbg != "" {
		var n int
		fmt.Sscanf(dbg, "%d", &n)
		fmt.Printf("%q\n", describe(corpus()[n]))
		return
	}
	var b bytes.Buffer
	b.WriteString(strings.TrimRight(os.Getenv("BURROW_XTEXT_TEST_BANNER"), "\n") + "\n\n")
	b.WriteString("#ifndef BURROW_TESTS_XTEXT_BIDIRULE_TEST_GEN_H\n#define BURROW_TESTS_XTEXT_BIDIRULE_TEST_GEN_H\n\n")
	b.WriteString("/* clang-format off */\n\n")
	writeTestData(t, &b)
	b.WriteString("\n/* clang-format on */\n\n#endif /* BURROW_TESTS_XTEXT_BIDIRULE_TEST_GEN_H */\n")
	if err := os.WriteFile(filepath.Join(testsDir, "xtext_bidirule_test_gen.h"), b.Bytes(), 0o644); err != nil {
		t.Fatal(err)
	}
}
