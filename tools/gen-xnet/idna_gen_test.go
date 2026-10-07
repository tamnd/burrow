// Laid over golang.org/x/text/internal/export/idna by tools/gen-xnet-idna.sh,
// so that it can read the package's unexported tables and test tables by name.
// That package is the source the copy under golang.org/x/net/idna is generated
// from, and the script checks that the two are the same code before it runs
// this. It writes three files: the C tables, a header with the constants that
// go with them, and the test data for tests/xnet_idna_test.c.
//
// Copyright 2026 The burrow Authors. All rights reserved.
// Use of this source code is governed by a BSD-style licence that can be found
// in the LICENSE file.

package idna

import (
	"bytes"
	"fmt"
	"hash/fnv"
	"io"
	"math/rand/v2"
	"os"
	"path/filepath"
	"reflect"
	"regexp"
	"strconv"
	"strings"
	"testing"

	"golang.org/x/text/internal/ucd"
)

// cstr is s as a C string literal, cut into pieces of at most 72 bytes of
// source so that the generated file stays readable. Octal escapes are always
// three digits, so a digit after one cannot be read as part of it.
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
			// No trigraphs.
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

// clit is a string literal with its length, for a struct initialiser.
func clit(s string) string {
	return fmt.Sprintf("%s, %d", cstr(s), len(s))
}

func carray(w io.Writer, ctype, name string, v reflect.Value, perLine int) {
	fmt.Fprintf(w, "const %s %s[%d] = {\n", ctype, name, v.Len())
	for i := 0; i < v.Len(); i++ {
		if i%perLine == 0 {
			fmt.Fprint(w, "    ")
		}
		fmt.Fprintf(w, "0x%x,", v.Index(i).Uint())
		if i%perLine == perLine-1 || i == v.Len()-1 {
			fmt.Fprintln(w)
		} else {
			fmt.Fprint(w, " ")
		}
	}
	fmt.Fprintln(w, "};")
	fmt.Fprintln(w)
}

func ctypeOf(v reflect.Value) string {
	switch v.Type().Elem().Size() {
	case 1:
		return "uint8_t"
	case 2:
		return "uint16_t"
	}
	panic("unexpected element size")
}

// cutoff reads the block count below which a trie block is a plain table,
// which the Go tables only have as a literal inside lookupValue.
func cutoff(t *testing.T) int {
	src, err := os.ReadFile("tables" + UnicodeVersion + ".go")
	if err != nil {
		t.Fatal(err)
	}
	re := regexp.MustCompile(`func \(t \*idnaTrie\) lookupValue[^{]*\{\s*switch \{\s*case n < (\d+):`)
	m := re.FindSubmatch(src)
	if m == nil {
		t.Fatal("no cutoff in lookupValue")
	}
	n, _ := strconv.Atoi(string(m[1]))
	return n
}

func writeTables(t *testing.T, srcDir, banner string) {
	var h bytes.Buffer
	fmt.Fprintf(&h, banner, "tables"+UnicodeVersion+".go")
	fmt.Fprintf(&h, "#ifndef BURROW_SRC_XNET_IDNA_TABLES_H\n#define BURROW_SRC_XNET_IDNA_TABLES_H\n\n")
	fmt.Fprintf(&h, "#include \"idna.h\"\n\n#include <stdint.h>\n\n")
	fmt.Fprintf(&h, "/* clang-format off */\n\n")
	fmt.Fprintf(&h, "#define IDNA_UNICODE_VERSION %q\n", UnicodeVersion)
	fmt.Fprintf(&h, "#define IDNA_CUTOFF %d\n\n", cutoff(t))
	fmt.Fprintf(&h, "/* One record of a sparse trie block. The first record of a block is a\n")
	fmt.Fprintf(&h, " * header, with the stride in value and the record count in lo. */\n")
	fmt.Fprintf(&h, "typedef struct IdnaValueRange {\n    uint16_t value;\n    uint8_t lo, hi;\n} IdnaValueRange;\n\n")

	type arr struct {
		name string
		v    reflect.Value
	}
	arrs := []arr{
		{"idna_mappings", reflect.ValueOf([]byte(mappings))},
		{"idna_mapping_index", reflect.ValueOf(mappingIndex)},
		{"idna_xor_data", reflect.ValueOf([]byte(xorData))},
		{"idna_values", reflect.ValueOf(idnaValues[:])},
		{"idna_index", reflect.ValueOf(idnaIndex[:])},
		{"idna_sparse_offset", reflect.ValueOf(idnaSparseOffset)},
	}
	for _, a := range arrs {
		fmt.Fprintf(&h, "extern const %s burrow__%s[%d];\n", ctypeOf(a.v), a.name, a.v.Len())
	}
	fmt.Fprintf(&h, "extern const IdnaValueRange burrow__idna_sparse_values[%d];\n\n", len(idnaSparseValues))
	fmt.Fprintf(&h, "/* clang-format on */\n\n#endif /* BURROW_SRC_XNET_IDNA_TABLES_H */\n")

	var c bytes.Buffer
	fmt.Fprintf(&c, banner, "tables"+UnicodeVersion+".go")
	fmt.Fprintf(&c, "#include \"idna.h\"\n#include \"idna_tables.h\"\n\n")
	fmt.Fprintf(&c, "/* clang-format off */\n\n")
	for _, a := range arrs {
		per := 16
		if a.v.Type().Elem().Size() == 2 {
			per = 10
		}
		carray(&c, ctypeOf(a.v), "burrow__"+a.name, a.v, per)
	}
	fmt.Fprintf(&c, "const IdnaValueRange burrow__idna_sparse_values[%d] = {\n", len(idnaSparseValues))
	for i, r := range idnaSparseValues {
		if i%4 == 0 {
			fmt.Fprint(&c, "    ")
		}
		fmt.Fprintf(&c, "{0x%04x, 0x%02x, 0x%02x},", r.value, r.lo, r.hi)
		if i%4 == 3 || i == len(idnaSparseValues)-1 {
			fmt.Fprintln(&c)
		} else {
			fmt.Fprint(&c, " ")
		}
	}
	fmt.Fprintf(&c, "};\n\n/* clang-format on */\n")

	write(t, filepath.Join(srcDir, "idna_tables.h"), h.Bytes())
	write(t, filepath.Join(srcDir, "idna_tables.c"), c.Bytes())
}

func write(t *testing.T, path string, b []byte) {
	if err := os.WriteFile(path, b, 0o644); err != nil {
		t.Fatal(err)
	}
}

// ---------------------------------------------------------------- test data

func writePunycode(w io.Writer) {
	fmt.Fprintf(w, "/* punycodeTestCases, from punycode_test.go. */\n")
	fmt.Fprintf(w, "static const IdnaPunycodeCase idna_punycode_cases[] = {\n")
	for _, tc := range punycodeTestCases {
		fmt.Fprintf(w, "    {%s,\n     %s},\n", clit(tc.s), clit(tc.encoded))
	}
	fmt.Fprintf(w, "};\n\n")
}

// conformance is one line of IdnaTestV2.txt with the defaults filled in the
// way conformancev2_test.go fills them, and what Go makes of it: bit 0 is set
// when Go's ToUnicode passes doTest, bit 1 the nontransitional ToASCII and bit
// 2 the transitional one.
type conformance struct {
	src, toUnicode, toUnicodeErr string
	toASCIIN, toASCIINErr        string
	toASCIIT, toASCIITErr        string
	goOK                         int
}

var (
	confTransitional    = New(Transitional(true), VerifyDNSLength(true), BidiRule(), MapForLookup())
	confNonTransitional = New(VerifyDNSLength(true), BidiRule(), MapForLookup())
)

// passes is doTest without the testing.T: whether f(input) gives want, when
// want is not empty, and an error whose code is in errors.
func passes(f func(string) (string, error), input, want, errors string) bool {
	errors = strings.Trim(errors, "[]")
	got, err := f(input)
	if err != nil {
		code := err.(interface{ code() string }).code()
		if strings.Index(errors, code) == -1 {
			return false
		}
	} else if errors != "" {
		return false
	}
	return want == "" || got == want
}

func readConformance(t *testing.T) []conformance {
	path := os.Getenv("BURROW_XNET_IDNA_TEST")
	f, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	var out []conformance
	p := ucd.New(f)
	for p.Next() {
		var c conformance
		c.src = def(unescape(p.String(0)), "")
		c.toUnicode = def(unescape(p.String(1)), c.src)
		c.toUnicodeErr = p.String(2)
		c.toASCIIN = def(unescape(p.String(3)), c.toUnicode)
		c.toASCIINErr = def(p.String(4), c.toUnicodeErr)
		c.toASCIIT = def(unescape(p.String(5)), c.toASCIIN)
		c.toASCIITErr = def(p.String(6), c.toASCIINErr)
		if passes(confNonTransitional.ToUnicode, c.src, c.toUnicode, c.toUnicodeErr) {
			c.goOK |= 1
		}
		if passes(confNonTransitional.ToASCII, c.src, c.toASCIIN, c.toASCIINErr) {
			c.goOK |= 2
		}
		if passes(confTransitional.ToASCII, c.src, c.toASCIIT, c.toASCIITErr) {
			c.goOK |= 4
		}
		out = append(out, c)
	}
	if err := p.Err(); err != nil {
		t.Fatal(err)
	}
	return out
}

func writeConformance(w io.Writer, cs []conformance) {
	fmt.Fprintf(w, "/* IdnaTestV2.txt from Unicode %s, with the empty fields filled in the\n", UnicodeVersion)
	fmt.Fprintf(w, " * way conformancev2_test.go fills them. The last field says which of the\n")
	fmt.Fprintf(w, " * three checks Go passes: 1 for ToUnicode, 2 for the nontransitional\n")
	fmt.Fprintf(w, " * ToASCII and 4 for the transitional one. */\n")
	fmt.Fprintf(w, "static const IdnaConformance idna_conformance[] = {\n")
	for _, c := range cs {
		fmt.Fprintf(w, "    {{%s},\n     {%s}, {%s},\n     {%s}, {%s},\n     {%s}, {%s},\n     %d},\n",
			clit(c.src), clit(c.toUnicode), clit(c.toUnicodeErr), clit(c.toASCIIN),
			clit(c.toASCIINErr), clit(c.toASCIIT), clit(c.toASCIITErr), c.goOK)
	}
	fmt.Fprintf(w, "};\n\n")
}

// ---------------------------------------------------------------- differential

type record struct{ buf bytes.Buffer }

func (r *record) s(tag string, s string) {
	fmt.Fprintf(&r.buf, "%s%d:%s", tag, len(s), s)
}

func (r *record) n(tag string, n int) { fmt.Fprintf(&r.buf, "%s%d;", tag, n) }

func (r *record) e(err error) {
	if err == nil {
		r.buf.WriteString("-;")
		return
	}
	code := err.(interface{ code() string }).code()
	fmt.Fprintf(&r.buf, "%s|%s;", code, err.Error())
}

func hash64(b []byte) uint64 {
	h := fnv.New64a()
	h.Write(b)
	return h.Sum64()
}

// The profiles the differential runs every corpus string through, in the
// order tests/xnet_idna_test.c builds them.
func diffProfiles() []*Profile {
	return []*Profile{
		Punycode,
		Lookup,
		Display,
		Registration,
		confNonTransitional,
		confTransitional,
		New(MapForLookup(), StrictDomainName(false)),
		New(MapForLookup(), CheckHyphens(false)),
		New(ValidateLabels(true)),
		New(ValidateForRegistration(), Transitional(true)),
		New(RemoveLeadingDots(true), MapForLookup(), VerifyDNSLength(true)),
		New(BidiRule()),
		New(MapForLookup(), CheckJoiners(false), BidiRule()),
	}
}

func describe(s string) []byte {
	var r record
	for _, p := range diffProfiles() {
		a, err := p.ToASCII(s)
		r.s("A", a)
		r.e(err)
		u, err := p.ToUnicode(s)
		r.s("U", u)
		r.e(err)
	}
	return r.buf.Bytes()
}

// palette is what the random strings are made of: the pieces IDNA has rules
// about, and some plain ones to put between them.
var palette = []string{
	"a", "b", "z", "Q", "0", "9", "-", "--", ".", ".", "xn--", "_", "*",
	"\u00df", "\u1e9e", "\u03c2", "\u00e9", "e\u0301", "\u0323", "\u0322",
	"\u200c", "\u200d", "\u094d", "\u0915", "\u0628", "\u0627", "\u0660",
	"\u05d0", "\u05b0", "\u0671", "\u07dc", "\u3002", "\uff0e", "\uff61",
	"\u2490", "\u2488", "\ufecb", "\ufb01", "\u2163", "\u00ad", "\u180c",
	"\U0001f600", "\U0001e942", "\U000e0181", "\ufffd", "\ufe05",
	"\xed", "\xff", "\xc3", "\xe2\x82",
}

func randomStrings() []string {
	rng := rand.New(rand.NewPCG(2143, 46))
	var out []string
	for i := 0; i < 1500; i++ {
		var b strings.Builder
		n := 1 + rng.IntN(12)
		if i%25 == 24 {
			n = 60 + rng.IntN(80)
		}
		for j := 0; j < n; j++ {
			b.WriteString(palette[rng.IntN(len(palette))])
		}
		out = append(out, b.String())
	}
	return out
}

// extras are the inputs of the tests that keep their cases inside the test
// function, where the generator cannot reach them, and some edge cases of the
// label iterator and the length checks.
var extras = []string{
	"", ".", "..", "a.", "a..", ".a", "..b", "b..", "a..b", "xn--", "foo.xn--",
	"xn--.foo", "foo.xn--.bar", "\u3002b", "\xed", "*.foo.com", "Foo.com",
	"r3---sn-apo3qvuoxuxbt-j5pe.googlevideo.com", "-label-.com",
	"lab\u2490be", "plan\u2490fa\u00df.de", "Plan\u2490fa\u00df.de", "Plan9fa\u00df.de",
	"\u65e5\u672c\u2488co.\u00df\u00df\u00df.de", "a\u200cb", "xn--ab-j1t",
	"gr\ufecb\ufeae\ufe91\ufef2.de", "\u0671.\u03c3\u07dc", "a\u0323\u0322",
	"xn--a-tdbc.com", "\ufe05\u3002\u3002\U0002603e\u1ce0",
	"FAX\u2a77\U0001d186\u3002\U0001e942\U000e0181\u180c", "stra\u00dfe.de",
	"books", "xn--bcher-kva", "foo--xn--bar.org", "golang.org", "example.xn--p1ai",
	"xn--czrw28b.tw", "www.xn--mller-kva.de", "b\u00fccher", "example.\u0440\u0444",
	"\u5546\u696d.tw", "www.m\u00fcller.de", "example\u3002jp", "\u6771\u4eac\uff0ejp",
	"\u5927\u962a\uff61jp", "*.G\u00d6PHER.com", "www.G\u00d6PHER.com", "www.g\u00f6pher.com",
	"*.fa\u00df.com", "www.golang.org", "xn--9", "xn--99999a", "xn--a-", "xn---",
	"xn--zz", "xn--ls8h", "xn--abc.xn--def", "a\xe2\x80", "\u1e9e", "xn--zca",
	strings.Repeat("a", 63), strings.Repeat("a", 64), strings.Repeat("a.", 126) + "a",
	strings.Repeat("abcdefghi.", 25) + "abc", strings.Repeat("abcdefghi.", 25) + "abcd",
	strings.Repeat("abcdefghi.", 25) + "abc.", strings.Repeat("\u00fc", 40),
}

func corpus(cs []conformance) []string {
	seen := map[string]bool{}
	var out []string
	add := func(s string) {
		if !seen[s] {
			seen[s] = true
			out = append(out, s)
		}
	}
	for _, c := range cs {
		add(c.src)
	}
	for _, s := range extras {
		add(s)
	}
	for _, tc := range punycodeTestCases {
		add(tc.s)
		add(acePrefix + tc.encoded)
	}
	for _, s := range randomStrings() {
		add(s)
	}
	return out
}

// sweepRecord is what the sweep says about r: its trie value and the length
// the lookup reads, and three profiles' answers about labels with r in them.
func sweepRecord(r rune) []byte {
	var rec record
	s := string(r)
	v, sz := trie.lookupString(s)
	rec.n("T", int(v))
	rec.n("", sz)
	a, err := Lookup.ToASCII("a" + s + ".b")
	rec.s("L", a)
	rec.e(err)
	u, err := Display.ToUnicode(s)
	rec.s("D", u)
	rec.e(err)
	a, err = confTransitional.ToASCII("x" + s)
	rec.s("T", a)
	rec.e(err)
	return rec.buf.Bytes()
}

func writeDifferential(w io.Writer, cs []conformance) {
	corp := corpus(cs)
	fmt.Fprintf(w, "/* The strings the C test runs through every profile in diff_profiles, and\n")
	fmt.Fprintf(w, " * the hash of what Go says about each. See describe in the generator. The\n")
	fmt.Fprintf(w, " * conformance inputs come first, then the rest. */\n")
	fmt.Fprintf(w, "static const IdnaLit idna_corpus[] = {\n")
	for _, s := range corp {
		fmt.Fprintf(w, "    {%s},\n", clit(s))
	}
	fmt.Fprintf(w, "};\n\nstatic const uint64_t idna_corpus_hash[] = {\n")
	for _, s := range corp {
		fmt.Fprintf(w, "    0x%016xU,\n", hash64(describe(s)))
	}
	fmt.Fprintf(w, "};\n\n")

	// The sweep: one hash per block of 0x1000 runes of the records of every
	// rune in the block, surrogates left out.
	fmt.Fprintf(w, "static const uint64_t idna_sweep_hash[0x110] = {\n")
	for blk := rune(0); blk < 0x110; blk++ {
		h := fnv.New64a()
		for r := blk << 12; r < (blk+1)<<12; r++ {
			if r >= 0xd800 && r < 0xe000 {
				continue
			}
			h.Write(sweepRecord(r))
		}
		fmt.Fprintf(w, "    0x%016xU,\n", h.Sum64())
	}
	fmt.Fprintf(w, "};\n")
}

func TestZZGen(t *testing.T) {
	srcDir := os.Getenv("BURROW_XNET_SRC")
	testsDir := os.Getenv("BURROW_XNET_TESTS")
	banner := os.Getenv("BURROW_XNET_BANNER")
	if srcDir == "" || testsDir == "" || banner == "" {
		t.Skip("run by tools/gen-xnet-idna.sh")
	}
	cs := readConformance(t)
	if dbg := os.Getenv("BURROW_XNET_DESCRIBE"); dbg != "" {
		// For chasing a mismatch: what Go says about corpus entry n, or about
		// rune n in the sweep when it starts with U+.
		if r, ok := strings.CutPrefix(dbg, "U+"); ok {
			n, _ := strconv.ParseUint(r, 16, 32)
			fmt.Printf("%q\n", sweepRecord(rune(n)))
			return
		}
		n, _ := strconv.Atoi(dbg)
		fmt.Printf("%q\n", describe(corpus(cs)[n]))
		return
	}
	writeTables(t, srcDir, banner)

	var b bytes.Buffer
	b.WriteString(os.Getenv("BURROW_XNET_TEST_BANNER"))
	b.WriteString("#ifndef BURROW_TESTS_XNET_IDNA_TEST_GEN_H\n#define BURROW_TESTS_XNET_IDNA_TEST_GEN_H\n\n")
	b.WriteString("/* clang-format off */\n\n")
	writePunycode(&b)
	writeConformance(&b, cs)
	writeDifferential(&b, cs)
	b.WriteString("\n/* clang-format on */\n\n#endif /* BURROW_TESTS_XNET_IDNA_TEST_GEN_H */\n")
	write(t, filepath.Join(testsDir, "xnet_idna_test_gen.h"), b.Bytes())
}
