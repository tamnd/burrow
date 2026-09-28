#!/bin/sh
# Regenerates tests/regexp_test_gen.h: the patterns and texts from Go's regexp
# tests, a sample of the RE2 search and exhaustive tests, the Fowler POSIX
# tests and a few thousand random cases, with a summary of what Go's regexp
# makes of each one. The summary is every Find, FindAll, Split, Replace and
# Expand form run on the case, written out as text, and tests/regexp_test.c
# writes the same summary with burrow and has to get the same text. Anything
# over 120 bytes is kept as its FNV-1a hash. The small tables that test
# internals, onePassTests and minInputLenTests, come along as they are.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/regexp_test_gen.h"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
goroot=$(go env GOROOT)

# Build inside a copy of the package, so the generator can use the test tables
# and the unexported compile, and hook the RE2 and Fowler test drivers to
# collect their cases.
mkdir "$tmp/regexp"
cp -R "$goroot/src/regexp/"*.go "$goroot/src/regexp/testdata" "$tmp/regexp/"
rm -f "$tmp/regexp/example_test.go"
printf 'module rxgen\n\ngo 1.26\n' > "$tmp/regexp/go.mod"
perl -0pi -e 's/(\t\t\tres := strings.Split\(line, ";"\)\n)/\t\t\tif re2Hook != nil {\n\t\t\t\tre2Hook(re, refull, text)\n\t\t\t}\n$1/' "$tmp/regexp/exec_test.go"
perl -0pi -e 's/(\t\t\tre, err := compile\(pattern, syn, true\)\n)/\t\t\tif fowlerHook != nil {\n\t\t\t\tfowlerHook(pattern, syn, text)\n\t\t\t}\n$1/' "$tmp/regexp/exec_test.go"
perl -0pi -e 's/\t"internal\/testenv"\n//; s/testenv\.Builder\(\)/""/g' "$tmp/regexp/exec_test.go"
grep -q re2Hook "$tmp/regexp/exec_test.go"
grep -q fowlerHook "$tmp/regexp/exec_test.go"
# The package imports regexp/syntax from the standard library, which is what
# the copy has to use too.
cat > "$tmp/regexp/gen_test.go" <<'GO'
package regexp

import (
	"fmt"
	"hash/fnv"
	"math/rand/v2"
	"os"
	"regexp/syntax"
	"strconv"
	"strings"
	"testing"
)

var (
	re2Hook    func(re, refull *Regexp, text string)
	fowlerHook func(pattern string, syn syntax.Flags, text string)
)

type genCase struct {
	pat     string
	flags   syntax.Flags
	longest bool
	text    string
	repl    string
	n       int
}

// cstr writes s as a C string literal, with everything outside printable
// ASCII, and the characters that mean something in one, as octal.
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
	}
	b.WriteByte('"')
	return b.String()
}

// rxText is a RxText: the string itself, or its hash when it is long.
func rxText(s string) string {
	if len(s) > 120 {
		h := fnv.New64a()
		h.Write([]byte(s))
		return fmt.Sprintf("{NULL, %d, 0x%016xULL}", len(s), h.Sum64())
	}
	return fmt.Sprintf("{%s, %d, 0}", cstr(s), len(s))
}

func q(s string) string { return strconv.Quote(s) }

func ints(a []int) string {
	if a == nil {
		return "nil"
	}
	return fmt.Sprint(a)
}

func intss(a [][]int) string {
	if a == nil {
		return "nil"
	}
	var parts []string
	for _, x := range a {
		parts = append(parts, ints(x))
	}
	return "[" + strings.Join(parts, " ") + "]"
}

func strs(a []string) string {
	if a == nil {
		return "nil"
	}
	var parts []string
	for _, x := range a {
		parts = append(parts, q(x))
	}
	return "[" + strings.Join(parts, " ") + "]"
}

func strss(a [][]string) string {
	if a == nil {
		return "nil"
	}
	var parts []string
	for _, x := range a {
		parts = append(parts, strs(x))
	}
	return "[" + strings.Join(parts, " ") + "]"
}

func bytes1(b []byte) string {
	if b == nil {
		return "nil"
	}
	return q(string(b))
}

func bytess(a [][]byte) string {
	if a == nil {
		return "nil"
	}
	var parts []string
	for _, x := range a {
		parts = append(parts, bytes1(x))
	}
	return "[" + strings.Join(parts, " ") + "]"
}

func bytesss(a [][][]byte) string {
	if a == nil {
		return "nil"
	}
	var parts []string
	for _, x := range a {
		parts = append(parts, bytess(x))
	}
	return "[" + strings.Join(parts, " ") + "]"
}

// summary is everything the API says about one case, one field a line.
func summary(c genCase) string {
	re, err := compile(c.pat, c.flags, c.longest)
	if err != nil {
		return "err=" + err.Error() + "\n"
	}
	var o strings.Builder
	t := c.text
	b := []byte(t)
	w := func(k, v string) { o.WriteString(k + "=" + v + "\n") }
	w("str", q(re.String()))
	w("num", fmt.Sprint(re.NumSubexp()))
	w("names", strs(re.SubexpNames()))
	var idx []int
	for _, n := range append(re.SubexpNames(), "missing") {
		idx = append(idx, re.SubexpIndex(n))
	}
	w("index", ints(idx))
	p, complete := re.LiteralPrefix()
	w("prefix", q(p)+" "+fmt.Sprint(complete))
	w("m", fmt.Sprint(re.MatchString(t), re.Match(b), re.MatchReader(strings.NewReader(t))))
	w("f", q(re.FindString(t))+" "+bytes1(re.Find(b)))
	w("fi", ints(re.FindStringIndex(t))+" "+ints(re.FindIndex(b))+" "+ints(re.FindReaderIndex(strings.NewReader(t))))
	si := re.FindStringSubmatchIndex(t)
	w("si", ints(si)+" "+ints(re.FindSubmatchIndex(b))+" "+ints(re.FindReaderSubmatchIndex(strings.NewReader(t))))
	w("s", strs(re.FindStringSubmatch(t))+" "+bytess(re.FindSubmatch(b)))
	w("a", strs(re.FindAllString(t, -1))+" "+bytess(re.FindAll(b, -1)))
	w("a2", strs(re.FindAllString(t, 2))+" "+bytess(re.FindAll(b, 2)))
	w("ai", intss(re.FindAllStringIndex(t, -1))+" "+intss(re.FindAllIndex(b, 2)))
	w("as", strss(re.FindAllStringSubmatch(t, -1))+" "+bytesss(re.FindAllSubmatch(b, 3)))
	w("asi", intss(re.FindAllStringSubmatchIndex(t, -1))+" "+intss(re.FindAllSubmatchIndex(b, 1)))
	w("sp", strs(re.Split(t, c.n))+" "+strs(re.Split(t, 2)))
	w("r", q(re.ReplaceAllString(t, c.repl))+" "+bytes1(re.ReplaceAll(b, []byte(c.repl))))
	w("rl", q(re.ReplaceAllLiteralString(t, c.repl))+" "+bytes1(re.ReplaceAllLiteral(b, []byte(c.repl))))
	paren := func(s string) string { return "(" + s + ")" }
	w("rf", q(re.ReplaceAllStringFunc(t, paren))+" "+bytes1(re.ReplaceAllFunc(b, func(s []byte) []byte { return []byte(paren(string(s))) })))
	if si != nil {
		w("e", bytes1(re.ExpandString(nil, c.repl, t, si))+" "+bytes1(re.Expand([]byte("<"), []byte(c.repl), b, si)))
	}
	w("q", q(QuoteMeta(t)))
	return o.String()
}

const defRepl = "<$0|$1|${2}x|$1x|$$|$name|${|$>"

var genTokens = []string{
	"a", "b", "c", "a", "b", "A", "B", "x", "y", "é", "☺", "0", "_", " ", "\n", ",", "-",
	".", ".", "^", "$", "|", "|", "(", "(", ")", ")", "(?:", "(?i)", "(?s)", "(?m)",
	"(?U)", "(?P<name>", "(?P<x1>", "*", "+", "?", "*?", "+?", "??", "{2}", "{0}",
	"{1,3}", "{2,}", "{0,1}", "[a-c]", "[^a]", "[ab]", "[[:alpha:]]", "\\d", "\\w",
	"\\W", "\\s", "\\S", "\\pL", "\\b", "\\B", "\\A", "\\z", "\\Q.*\\E", "\\x41", "\\.",
}

var genAlphabet = []string{"a", "a", "b", "b", "c", "A", "B", "x", "y", "é", "☺", "0", "_", " ", "\n", ",", "-", ".", "\xff"}

func TestGen(t *testing.T) {
	path := os.Getenv("RXGEN_OUT")
	if path == "" {
		t.Skip("RXGEN_OUT not set")
	}
	var cases []genCase
	seen := map[genCase]bool{}
	add := func(c genCase) {
		if !seen[c] && len(c.pat) <= 2000 {
			seen[c] = true
			cases = append(cases, c)
		}
	}
	perl := func(pat, text string) {
		add(genCase{pat, syntax.Perl, false, text, defRepl, -1})
	}
	for _, p := range goodRe {
		perl(p, "")
		perl(p, "abc")
		perl(p, "a!\\b[c]\n-z")
	}
	for _, e := range badRe {
		if len(e.re) < 100 {
			perl(e.re, "")
		}
	}
	for _, tt := range findTests {
		perl(tt.pat, tt.text)
		add(genCase{tt.pat, syntax.Perl, true, tt.text, defRepl, -1})
	}
	for _, tt := range replaceTests {
		add(genCase{tt.pattern, syntax.Perl, false, tt.input, tt.replacement, -1})
	}
	for _, tt := range replaceLiteralTests {
		add(genCase{tt.pattern, syntax.Perl, false, tt.input, tt.replacement, -1})
	}
	for _, tt := range replaceFuncTests {
		perl(tt.pattern, tt.input)
	}
	for _, tt := range metaTests {
		perl(tt.pattern, tt.pattern)
		perl(QuoteMeta(tt.pattern), tt.pattern)
	}
	for _, tt := range literalPrefixTests {
		perl(tt.pattern, tt.pattern)
	}
	for _, tt := range subexpCases {
		perl(tt.input, "xabbayabaz")
	}
	for _, tt := range splitTests {
		add(genCase{tt.r, syntax.Perl, false, tt.s, defRepl, tt.n})
	}
	for _, tt := range onePassTests {
		if len(tt.re) < 100 {
			perl(tt.re, "abcd")
			perl(tt.re, "aa")
		}
	}
	for _, tt := range onePassTests1 {
		perl(tt.re, tt.match)
	}
	for _, tt := range minInputLenTests {
		perl(tt.Regexp, "aaaaaa日")
	}

	// A sample of the RE2 search tests and of the exhaustive ones, each
	// pattern both anchored and not, and leftmost first or longest turn about.
	ncase := 0
	sample := 3
	re2Hook = func(re, refull *Regexp, text string) {
		ncase++
		if ncase%sample != 0 {
			return
		}
		add(genCase{re.String(), syntax.Perl, ncase%2 == 0, text, defRepl, -1})
		add(genCase{refull.String(), syntax.Perl, ncase%2 == 1, text, defRepl, -1})
	}
	testRE2(t, "testdata/re2-search.txt")
	sample = 997
	testRE2(t, "testdata/re2-exhaustive.txt.bz2")
	re2Hook = nil

	fowlerHook = func(pattern string, syn syntax.Flags, text string) {
		add(genCase{pattern, syn, true, text, defRepl, -1})
	}
	for _, f := range []string{"testdata/basic.dat", "testdata/nullsubexpr.dat", "testdata/repetition.dat"} {
		testFowler(t, f)
	}
	fowlerHook = nil

	r := rand.New(rand.NewPCG(3, 4))
	flags := []syntax.Flags{syntax.Perl, syntax.Perl, syntax.Perl, syntax.POSIX, syntax.Perl | syntax.FoldCase}
	for len(cases) < 16000 {
		var b strings.Builder
		for i, n := 0, 1+r.IntN(8); i < n; i++ {
			b.WriteString(genTokens[r.IntN(len(genTokens))])
		}
		pat := b.String()
		f := flags[r.IntN(len(flags))]
		longest := r.IntN(3) == 0
		for k := 0; k < 3; k++ {
			var tb strings.Builder
			for i, n := 0, r.IntN(12); i < n; i++ {
				tb.WriteString(genAlphabet[r.IntN(len(genAlphabet))])
			}
			add(genCase{pat, f, longest, tb.String(), defRepl, -1})
		}
	}

	var o strings.Builder
	o.WriteString("/* Generated by tools/gen-regexp-tests.sh from Go's regexp. Do not edit. */\n\n/* clang-format off */\n")
	o.WriteString("static const RxGenCase rx_gen_cases[] = {\n")
	for _, c := range cases {
		lg := 0
		if c.longest {
			lg = 1
		}
		fmt.Fprintf(&o, "{%s, %d, 0x%x, %d, %s, %d, %s, %d, %d, %s},\n", cstr(c.pat), len(c.pat), uint16(c.flags), lg,
			cstr(c.text), len(c.text), cstr(c.repl), len(c.repl), c.n, rxText(summary(c)))
	}
	o.WriteString("};\n\nstatic const RxOnePassCase rx_onepass_cases[] = {\n")
	for _, tt := range onePassTests {
		b := 0
		if tt.isOnePass {
			b = 1
		}
		fmt.Fprintf(&o, "{%s, %d, %d},\n", cstr(tt.re), len(tt.re), b)
	}
	o.WriteString("};\n\nstatic const RxMinLenCase rx_min_len_cases[] = {\n")
	for _, tt := range minInputLenTests {
		fmt.Fprintf(&o, "{%s, %d, %d},\n", cstr(tt.Regexp), len(tt.Regexp), tt.min)
	}
	o.WriteString("};\n/* clang-format on */\n")
	if err := os.WriteFile(path, []byte(o.String()), 0o644); err != nil {
		t.Fatal(err)
	}
}
GO

(cd "$tmp/regexp" && RXGEN_OUT="$out" GOFLAGS=-mod=mod go test -run '^TestGen$' -count=1 . >/dev/null)
if command -v clang-format >/dev/null 2>&1; then
	clang-format -i "$out"
fi
