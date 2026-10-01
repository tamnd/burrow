// Laid over golang.org/x/text/unicode/norm by tools/gen-xtext-norm.sh, so that
// it can read the package's unexported tables and test tables by name. It
// writes three files: the C tables, a header with the constants that go with
// them, and the test data for tests/xtext_norm_test.c.
//
// Copyright 2026 The burrow Authors. All rights reserved.
// Use of this source code is governed by a BSD-style licence that can be found
// in the LICENSE file.

package norm

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"hash/fnv"
	"io"
	"math/rand/v2"
	"os"
	"path/filepath"
	"reflect"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"testing"
	"unicode/utf8"

	"golang.org/x/text/transform"
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

func carray(w io.Writer, ctype, name string, v reflect.Value, perLine int, hex bool) {
	fmt.Fprintf(w, "const %s %s[%d] = {\n", ctype, name, v.Len())
	for i := 0; i < v.Len(); i++ {
		if i%perLine == 0 {
			fmt.Fprint(w, "    ")
		}
		x := v.Index(i).Uint()
		if hex {
			fmt.Fprintf(w, "0x%x,", x)
		} else {
			fmt.Fprintf(w, "%d,", x)
		}
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

func sparse(w io.Writer, name string, vals []valueRange) {
	fmt.Fprintf(w, "const NormValueRange %s[%d] = {\n", name, len(vals))
	for i, r := range vals {
		if i%4 == 0 {
			fmt.Fprint(w, "    ")
		}
		fmt.Fprintf(w, "{0x%04x, 0x%02x, 0x%02x},", r.value, r.lo, r.hi)
		if i%4 == 3 || i == len(vals)-1 {
			fmt.Fprintln(w)
		} else {
			fmt.Fprint(w, " ")
		}
	}
	fmt.Fprintln(w, "};")
	fmt.Fprintln(w)
}

// cutoffs reads the block counts below which a trie block is a plain table,
// which the Go tables only have as literals inside lookupValue.
func cutoffs(t *testing.T) (nfc, nfkc int) {
	src, err := os.ReadFile("tables" + Version + ".go")
	if err != nil {
		t.Fatal(err)
	}
	re := regexp.MustCompile(`func \(t \*(nfc|nfkc)Trie\) lookupValue[^{]*\{\s*switch \{\s*case n < (\d+):`)
	for _, m := range re.FindAllSubmatch(src, -1) {
		n, _ := strconv.Atoi(string(m[2]))
		if string(m[1]) == "nfc" {
			nfc = n
		} else {
			nfkc = n
		}
	}
	if nfc == 0 || nfkc == 0 {
		t.Fatal("did not find the trie cutoffs")
	}
	return nfc, nfkc
}

func writeTables(t *testing.T, srcDir, banner string) {
	nfcCut, nfkcCut := cutoffs(t)

	var h bytes.Buffer
	fmt.Fprintf(&h, banner, "tables"+Version+".go")
	fmt.Fprintf(&h, "#ifndef BURROW_SRC_XTEXT_NORM_TABLES_H\n#define BURROW_SRC_XTEXT_NORM_TABLES_H\n\n")
	fmt.Fprintf(&h, "#include \"norm.h\"\n\n#include <stdint.h>\n\n")
	fmt.Fprintf(&h, "/* clang-format off */\n\n")
	fmt.Fprintf(&h, "#define NORM_UNICODE_VERSION %q\n", Version)
	fmt.Fprintf(&h, "#define NORM_MAX_TRANSFORM_CHUNK_SIZE %d\n\n", MaxTransformChunkSize)
	for _, c := range []struct {
		name string
		v    int
	}{
		{"FIRST_MULTI", firstMulti},
		{"FIRST_CCC", firstCCC},
		{"END_MULTI", endMulti},
		{"FIRST_LEADING_CCC", firstLeadingCCC},
		{"FIRST_CCC_ZERO_EXCEPT", firstCCCZeroExcept},
		{"FIRST_STARTER_WITH_N_LEAD", firstStarterWithNLead},
		{"LAST_DECOMP", lastDecomp},
		{"MAX_DECOMP", maxDecomp},
		{"NFC_CUTOFF", nfcCut},
		{"NFKC_CUTOFF", nfkcCut},
	} {
		fmt.Fprintf(&h, "#define NORM_%s 0x%X\n", c.name, c.v)
	}
	fmt.Fprintf(&h, "\n/* One record of a sparse trie block. The first record of a block is a\n")
	fmt.Fprintf(&h, " * header, with the stride in value and the record count in lo. */\n")
	fmt.Fprintf(&h, "typedef struct NormValueRange {\n    uint16_t value;\n    uint8_t lo, hi;\n} NormValueRange;\n\n")

	recomp := map[uint32]uint32{}
	for i := 0; i < len(recompMapPacked); i += 8 {
		b := []byte(recompMapPacked[i : i+8])
		recomp[binary.BigEndian.Uint32(b[:4])] = binary.BigEndian.Uint32(b[4:])
	}
	keys := make([]uint32, 0, len(recomp))
	for k := range recomp {
		keys = append(keys, k)
	}
	sort.Slice(keys, func(i, j int) bool { return keys[i] < keys[j] })

	type arr struct {
		name string
		v    reflect.Value
		hex  bool
	}
	arrs := []arr{
		{"norm_ccc_table", reflect.ValueOf(ccc[:]), false},
		{"norm_decomps", reflect.ValueOf(decomps[:]), true},
		{"norm_nfc_values", reflect.ValueOf(nfcValues[:]), true},
		{"norm_nfc_index", reflect.ValueOf(nfcIndex[:]), true},
		{"norm_nfc_sparse_offset", reflect.ValueOf(nfcSparseOffset[:]), true},
		{"norm_nfkc_values", reflect.ValueOf(nfkcValues[:]), true},
		{"norm_nfkc_index", reflect.ValueOf(nfkcIndex[:]), true},
		{"norm_nfkc_sparse_offset", reflect.ValueOf(nfkcSparseOffset[:]), true},
	}
	for _, a := range arrs {
		fmt.Fprintf(&h, "extern const %s burrow__%s[%d];\n", ctypeOf(a.v), a.name, a.v.Len())
	}
	fmt.Fprintf(&h, "extern const NormValueRange burrow__norm_nfc_sparse_values[%d];\n", len(nfcSparseValues))
	fmt.Fprintf(&h, "extern const NormValueRange burrow__norm_nfkc_sparse_values[%d];\n", len(nfkcSparseValues))
	fmt.Fprintf(&h, "\n/* The canonical compositions, as a<<16|b to the composed rune, sorted by\n")
	fmt.Fprintf(&h, " * key. a and b are cut to 16 bits first, the same as Go's combine. */\n")
	fmt.Fprintf(&h, "#define NORM_RECOMP_LEN %d\n", len(keys))
	fmt.Fprintf(&h, "extern const uint32_t burrow__norm_recomp[NORM_RECOMP_LEN][2];\n\n")
	fmt.Fprintf(&h, "/* clang-format on */\n\n#endif /* BURROW_SRC_XTEXT_NORM_TABLES_H */\n")

	var c bytes.Buffer
	fmt.Fprintf(&c, banner, "tables"+Version+".go")
	fmt.Fprintf(&c, "#include \"norm.h\"\n#include \"norm_tables.h\"\n\n")
	fmt.Fprintf(&c, "/* clang-format off */\n\n")
	for _, a := range arrs {
		per := 16
		if a.hex && a.v.Type().Elem().Size() == 2 {
			per = 10
		}
		carray(&c, ctypeOf(a.v), "burrow__"+a.name, a.v, per, a.hex)
	}
	sparse(&c, "burrow__norm_nfc_sparse_values", nfcSparseValues[:])
	sparse(&c, "burrow__norm_nfkc_sparse_values", nfkcSparseValues[:])
	fmt.Fprintf(&c, "const uint32_t burrow__norm_recomp[NORM_RECOMP_LEN][2] = {\n")
	for i, k := range keys {
		if i%3 == 0 {
			fmt.Fprint(&c, "    ")
		}
		fmt.Fprintf(&c, "{0x%08X, 0x%05X},", k, recomp[k])
		if i%3 == 2 || i == len(keys)-1 {
			fmt.Fprintln(&c)
		} else {
			fmt.Fprint(&c, " ")
		}
	}
	fmt.Fprintf(&c, "};\n\n/* clang-format on */\n")

	write(t, filepath.Join(srcDir, "norm_tables.h"), h.Bytes())
	write(t, filepath.Join(srcDir, "norm_tables.c"), c.Bytes())
}

func write(t *testing.T, path string, b []byte) {
	if err := os.WriteFile(path, b, 0o644); err != nil {
		t.Fatal(err)
	}
}

// ---------------------------------------------------------------- test data

var fnames = []string{"NORM_NFC", "NORM_NFD", "NORM_NFKC", "NORM_NFKD"}

func errCode(err error) int {
	switch err {
	case nil:
		return 0
	case transform.ErrShortDst:
		return 1
	case transform.ErrShortSrc:
		return 2
	case transform.ErrEndOfSpan:
		return 3
	}
	panic(err)
}

func posTable(w io.Writer, name string, tests []PositionTest) {
	fmt.Fprintf(w, "static const NormPositionTest %s[] = {\n", name)
	for _, tc := range tests {
		fmt.Fprintf(w, "    {%s,\n     %d, %s},\n", clit(tc.input), tc.pos, clit(tc.buffer))
	}
	fmt.Fprintf(w, "};\n\n")
}

func spanTable(w io.Writer, name string, tests []spanTest) {
	fmt.Fprintf(w, "static const NormSpanTest %s[] = {\n", name)
	for _, tc := range tests {
		fmt.Fprintf(w, "    {%s, %v, %d, %d},\n", clit(tc.input), tc.atEOF, tc.n, errCode(tc.err))
	}
	fmt.Fprintf(w, "};\n\n")
}

func appendTable(w io.Writer, name string, tests []AppendTest) {
	fmt.Fprintf(w, "static const NormAppendTest %s[] = {\n", name)
	for _, tc := range tests {
		fmt.Fprintf(w, "    {%s,\n     %s,\n     %s},\n", clit(tc.left), clit(tc.right), clit(tc.out))
	}
	if len(tests) == 0 {
		fmt.Fprintf(w, "    {\"\", 0, \"\", 0, \"\", 0},\n")
	}
	fmt.Fprintf(w, "};\n")
	fmt.Fprintf(w, "#define %s_LEN %d\n\n", strings.ToUpper(name), len(tests))
}

func runes(rs []rune) string {
	var b strings.Builder
	b.WriteString("{")
	for i, r := range rs {
		if i > 0 {
			b.WriteString(", ")
		}
		fmt.Fprintf(&b, "0x%X", r)
	}
	b.WriteString("}")
	return b.String()
}

func caseTable(w io.Writer, name string, tests []TestCase) {
	fmt.Fprintf(w, "static const NormRuneTest %s[] = {\n", name)
	for _, tc := range tests {
		fmt.Fprintf(w, "    {%d, %s, %d, %s},\n", len(tc.in), runes(tc.in), len(tc.out), runes(tc.out))
	}
	fmt.Fprintf(w, "};\n\n")
}

func segmentTable(w io.Writer, name string, tests []SegmentTest) {
	for i, tc := range tests {
		fmt.Fprintf(w, "static const NormLit %s_%d[] = {\n", name, i)
		for _, s := range tc.out {
			fmt.Fprintf(w, "    {%s},\n", clit(s))
		}
		fmt.Fprintf(w, "};\n")
	}
	fmt.Fprintf(w, "static const NormSegmentTest %s[] = {\n", name)
	for i, tc := range tests {
		fmt.Fprintf(w, "    {%s,\n     %s_%d, %d},\n", clit(tc.in), name, i, len(tc.out))
	}
	fmt.Fprintf(w, "};\n\n")
}

// The cases of TestNextBoundary and TestTransform, which are local to their
// functions in Go and so cannot be read by name. Copied from
// normalize_test.go and transform_test.go.
var nextBoundaryTests = []struct {
	input string
	atEOF bool
	want  int
}{
	{"", true, 0},
	{"", false, -1},
	{"\u0300", true, 2},
	{"\u0300", false, -1},
	{"\x80\x80", true, 1},
	{"\x80\x80", false, 1},
	{"\xff", false, 1},
	{"\u0300\xff", false, 2},
	{"\u0300\xc0\x80\x80", false, 2},
	{"\xc2\x80\x80", false, 2},
	{"\xc2", false, -1},
	{"\xc2", true, 1},
	{"a\u0300\xc2", false, -1},
	{"a\u0300\xc2", true, 3},
	{"a", true, 1},
	{"a", false, -1},
	{"aa", false, 1},
	{"\u0300", true, 2},
	{"\u0300", false, -1},
	{"\u0300a", false, 2},
	{"\u1103\u1161", true, 6},
	{"\u1103\u1161", false, -1},
	{"\u110B\u1173\u11B7", false, -1},
	{"\u110B\u1173\u11B7\u110B\u1173\u11B7", false, 9},
	{"\u1161\u110B\u1173\u11B7", false, 3},
	{"\u1173\u11B7\u1103\u1161", false, 6},
	{grave(maxNonStarters - 1), false, -1},
	{grave(maxNonStarters), false, 60},
	{grave(maxNonStarters + 1), false, 60},
}

var transformTests = []struct {
	f       Form
	in, out string
	eof     bool
	dstSize int
	err     error
}{
	{NFC, "ab", "ab", true, 2, nil},
	{NFC, "qx", "qx", true, 2, nil},
	{NFD, "qx", "qx", true, 2, nil},
	{NFC, "", "", true, 1, nil},
	{NFD, "", "", true, 1, nil},
	{NFC, "", "", false, 1, nil},
	{NFD, "", "", false, 1, nil},
	{NFD, "ö", "", true, 1, transform.ErrShortDst},
	{NFD, "ö", "", true, 2, transform.ErrShortDst},
	{NFC, "ab", "", true, 1, transform.ErrShortDst},
	{NFC, "qx", "", true, 1, transform.ErrShortDst},
	{NFC, "a\u0300abc", "\u00e0a", true, 4, transform.ErrShortDst},
	{NFD, "ö", "", false, 3, transform.ErrShortSrc},
	{NFC, "a\u0300", "", false, 4, transform.ErrShortSrc},
	{NFD, "a\u0300", "", false, 4, transform.ErrShortSrc},
	{NFC, "ö", "", false, 3, transform.ErrShortSrc},
	{NFD, "abc", "", false, 1, transform.ErrShortDst},
	{NFC, "abc", "", false, 1, transform.ErrShortDst},
	{NFC, "abc", "a", false, 2, transform.ErrShortDst},
	{NFD, "éfff", "", false, 2, transform.ErrShortDst},
	{NFC, "e\u0301fffff", "\u00e9fff", false, 6, transform.ErrShortDst},
	{NFD, "ééééé", "e\u0301e\u0301e\u0301", false, 15, transform.ErrShortDst},
	{NFC, "a\u0300", "", true, 1, transform.ErrShortDst},
	{NFC, "a\u0300", "", true, 2, transform.ErrShortDst},
	{NFC, "a\u0300", "", true, 3, transform.ErrShortDst},
	{NFC, "a\u0300", "\u00e0", true, 4, nil},
	{NFD, "öa\u0300", "o\u0308", false, 8, transform.ErrShortSrc},
	{NFD, "öa\u0300ö", "o\u0308a\u0300", true, 8, transform.ErrShortDst},
	{NFD, "öa\u0300ö", "o\u0308a\u0300", false, 12, transform.ErrShortSrc},
	{NFD, "\xbd\xb2=\xbc ", "\xbd\xb2=\xbc ", true, 8, nil},
}

// compareTests from example_iter_test.go, which is in package norm_test.
var exampleCompareTests = []struct{ a, b string }{
	{"aaa", "aaa"},
	{"aaa", "aab"},
	{"a\u0300a", "\u00E0a"},
	{"a\u0300\u0320b", "a\u0320\u0300b"},
	{"\u1E0A\u0323", "\x44\u0323\u0307"},
	{"\u3304", "\u30A4\u30CB\u30F3\u30AF\u3099"},
}

func writeTestData(t *testing.T, w io.Writer) {
	// TestProperties.
	fmt.Fprintf(w, "static const NormRuneData norm_test_data[] = {\n")
	for _, d := range testData {
		fmt.Fprintf(w, "    {0x%X, %d, %d, %d, {{%d, %v, %s}, {%d, %v, %s}}},\n", d.r, d.ccc, d.nLead, d.nTrail,
			d.f[0].qc, d.f[0].combinesForward, clit(d.f[0].decomposition),
			d.f[1].qc, d.f[1].combinesForward, clit(d.f[1].decomposition))
	}
	fmt.Fprintf(w, "};\n\n")

	posTable(w, "decompose_segment_tests", decomposeSegmentTests)
	posTable(w, "first_boundary_tests", firstBoundaryTests)
	posTable(w, "decompose_to_last_tests", decomposeToLastTests)
	posTable(w, "last_boundary_tests", lastBoundaryTests)
	posTable(w, "is_normal_tests", isNormalTests)
	posTable(w, "is_normal_nfd_tests", isNormalNFDTests)
	posTable(w, "is_normal_nfc_tests", isNormalNFCTests)
	posTable(w, "is_normal_nfkx_tests", isNormalNFKXTests)

	spanTable(w, "quick_span_tests", quickSpanTests)
	spanTable(w, "quick_span_nfd_tests", quickSpanNFDTests)
	spanTable(w, "quick_span_nfc_tests", quickSpanNFCTests)

	appendTable(w, "append_tests_nfc", appendTestsNFC)
	appendTable(w, "append_tests_nfd", appendTestsNFD)
	appendTable(w, "append_tests_nfkc", appendTestsNFKC)
	appendTable(w, "append_tests_nfkd", appendTestsNFKD)

	caseTable(w, "insert_tests", insertTests)
	caseTable(w, "decomposition_nfd_test", decompositionNFDTest)
	caseTable(w, "decomposition_nfkd_test", decompositionNFKDTest)
	caseTable(w, "composition_test", compositionTest)

	segmentTable(w, "segment_tests", segmentTests)
	segmentTable(w, "segment_tests_k", segmentTestsK)

	fmt.Fprintf(w, "static const NormNextBoundaryTest next_boundary_tests[] = {\n")
	for _, tc := range nextBoundaryTests {
		fmt.Fprintf(w, "    {%s, %v, %d},\n", clit(tc.input), tc.atEOF, tc.want)
	}
	fmt.Fprintf(w, "};\n\n")

	fmt.Fprintf(w, "static const NormTransformTest transform_tests[] = {\n")
	for _, tc := range transformTests {
		fmt.Fprintf(w, "    {%s, %s,\n     %s, %v, %d, %d},\n", fnames[tc.f], clit(tc.in), clit(tc.out), tc.eof, tc.dstSize, errCode(tc.err))
	}
	fmt.Fprintf(w, "};\n\n")

	fmt.Fprintf(w, "static const NormLit compare_tests[][2] = {\n")
	for _, tc := range exampleCompareTests {
		fmt.Fprintf(w, "    {{%s}, {%s}},\n", clit(tc.a), clit(tc.b))
	}
	fmt.Fprintf(w, "};\n\n")

	for _, tx := range []struct{ name, s string }{
		{"txt_canon", txt_canon}, {"txt_vn", txt_vn}, {"txt_ru", txt_ru}, {"txt_gr", txt_gr},
		{"txt_ar", txt_ar}, {"txt_il", txt_il}, {"txt_kr", txt_kr}, {"txt_th", txt_th},
		{"txt_jp", txt_jp}, {"txt_cn", txt_cn},
	} {
		fmt.Fprintf(w, "#define %s_LIT \\\n    %s\n", strings.ToUpper(tx.name), strings.ReplaceAll(cstr(tx.s), "\n        ", " \\\n    "))
	}
	fmt.Fprintln(w)
}

// ---------------------------------------------------------------- differential

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

var transformDstSizes = []int{0, 1, 2, 3, 4, 5, 8, 13, 32, MaxTransformChunkSize}
var ioSizes = []int{1, 3, 7, 4000}

// oneByteReader hands out at most n bytes a call, which is what makes the
// reader see a rune cut in two.
type chunkReader struct {
	b []byte
	n int
}

func (r *chunkReader) Read(p []byte) (int, error) {
	if len(r.b) == 0 {
		return 0, io.EOF
	}
	m := min(len(p), r.n, len(r.b))
	copy(p, r.b[:m])
	r.b = r.b[m:]
	return m, nil
}

// exact is []byte(s) with a capacity of exactly len(s).
func exact(s string) []byte {
	b := make([]byte, len(s))
	copy(b, s)
	return b
}

// catch runs fn and records the panic, if it panics. Go's norm panics on some
// inputs, an index out of range in the reorder buffer, and the C port panics
// with the same text at the same point.
func catch(r *record, fn func()) {
	defer func() {
		if p := recover(); p != nil {
			fmt.Fprintf(&r.buf, "PANIC%v;", p)
		}
	}()
	fn()
}

func describe(f Form, s string) []byte {
	var r record
	b := []byte(s)
	catch(&r, func() { r.s("S", []byte(f.String(s))) })
	catch(&r, func() { r.s("B", f.Bytes(b)) })
	catch(&r, func() { r.bool("N", f.IsNormal(b)) })
	catch(&r, func() { r.bool("NS", f.IsNormalString(s)) })
	catch(&r, func() { r.n("Q", f.QuickSpan(b)) })
	catch(&r, func() { r.n("QS", f.QuickSpanString(s)) })
	for _, eof := range []bool{false, true} {
		catch(&r, func() {
			n, err := f.Span(b, eof)
			r.n("SP", n)
			r.e("", err)
		})
		catch(&r, func() {
			n, err := f.SpanString(s, eof)
			r.n("SPS", n)
			r.e("", err)
		})
		catch(&r, func() { r.n("NB", f.NextBoundary(b, eof)) })
		catch(&r, func() { r.n("NBS", f.NextBoundaryInString(s, eof)) })
	}
	catch(&r, func() { r.n("FB", f.FirstBoundary(b)) })
	catch(&r, func() { r.n("FBS", f.FirstBoundaryInString(s)) })
	catch(&r, func() { r.n("LB", f.LastBoundary(b)) })

	catch(&r, func() {
		var it Iter
		it.Init(f, b)
		for !it.Done() {
			r.s("I", it.Next())
		}
	})
	catch(&r, func() {
		var it Iter
		it.InitString(f, s)
		for !it.Done() {
			r.s("IS", it.Next())
		}
	})

	dst := make([]byte, MaxTransformChunkSize)
	for _, sz := range transformDstSizes {
		for _, eof := range []bool{false, true} {
			catch(&r, func() {
				nDst, nSrc, err := f.Transform(dst[:sz], b, eof)
				r.n("T", nDst)
				r.n("", nSrc)
				r.e("", err)
				r.s("", dst[:nDst])
			})
		}
	}

	splits := []int{}
	if len(s) <= 64 {
		for i := 0; i <= len(s); i++ {
			splits = append(splits, i)
		}
	} else {
		for i := 0; i <= 16; i++ {
			splits = append(splits, i*len(s)/16)
		}
	}
	for _, i := range splits {
		// Append wants out to be normalised already, so the second one is
		// outside what it promises anything about. It is here because the
		// port should do what Go does even so.
		//
		// The prefix is a copy with its capacity equal to its length. When
		// Append patches up the rune at the join it holds a slice into out
		// while it appends to out, so what it produces can depend on how much
		// room out has, and []byte(s) leaves that up to the compiler.
		catch(&r, func() { r.s("A", f.Append(f.Bytes(exact(s[:i])), b[i:]...)) })
		catch(&r, func() { r.s("AR", f.Append(exact(s[:i]), b[i:]...)) })
		catch(&r, func() { r.s("AS", f.AppendString(f.Bytes(exact(s[:i])), s[i:])) })
	}

	for _, sz := range ioSizes {
		catch(&r, func() {
			rd := f.Reader(&chunkReader{b, sz})
			buf := make([]byte, sz)
			var out []byte
			for {
				n, err := rd.Read(buf)
				out = append(out, buf[:n]...)
				if err != nil {
					r.e("RE", err)
					break
				}
			}
			r.s("R", out)
		})
		catch(&r, func() {
			var wb bytes.Buffer
			wr := f.Writer(&wb)
			for in := b; len(in) > 0; {
				m := min(sz, len(in))
				wr.Write(in[:m])
				in = in[m:]
			}
			wr.Close()
			r.s("W", wb.Bytes())
		})
	}
	return r.buf.Bytes()
}

// The runes the random strings are made from: starters, combining marks of
// different classes, the Hangul pieces, runes with long or multi-segment
// decompositions, runes that compose, and bytes that are not UTF-8.
var palette = []string{
	"a", "e", "o", "A", "D", " ", "!", "\u00E0",
	"\u00C5", "\u00F6", "\u1E0A", "\u1E9B", "\u0300", "\u0301", "\u0302", "\u0308",
	"\u0316", "\u0323", "\u0327", "\u0335", "\u0344", "\u034F", "\u035B", "\u035D",
	"\u0F71", "\u0F72", "\u0F73", "\u0F74", "\u0F81", "\u05AE", "\u0315", "\u093C",
	"\u3099", "\uFF9E", "\u3332", "\uFDFA", "\u320E", "\u01C4", "\u1D16", "\u0958",
	"\u2126", "\u212B", "\u1100", "\u1161", "\u11A8", "\u110B", "\u1173", "\u11B7",
	"\uAC00", "\uAC01", "\uD7A3", "\u30D5", "\u30C8", "\u0635", "\u0644", "\u03B9",
	"\u0391", "\U0001D15E", "\U0001D164", "\U000110AB", "\U000110BA", "\U0002F800", "\u2000", "\u00A8",
	"\xff", "\x80", "\xc2", "\xe1\x84", "\xf0\x9d\x85", "\xed\xa0\x80", "\u0345", "\u1FBE",
}

func randomStrings() []string {
	rng := rand.New(rand.NewPCG(2143, 17))
	var out []string
	for i := 0; i < 600; i++ {
		var b strings.Builder
		n := 1 + rng.IntN(24)
		if i%10 == 9 {
			n = 30 + rng.IntN(60)
		}
		for j := 0; j < n; j++ {
			if i%7 == 6 && rng.IntN(3) == 0 {
				// A run of combining marks long enough to need a CGJ.
				m := 25 + rng.IntN(12)
				for k := 0; k < m; k++ {
					b.WriteString(palette[12+rng.IntN(8)])
				}
				continue
			}
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
	for _, tt := range [][]PositionTest{decomposeSegmentTests, firstBoundaryTests, decomposeToLastTests,
		lastBoundaryTests, isNormalTests, isNormalNFDTests, isNormalNFCTests, isNormalNFKXTests} {
		for _, tc := range tt {
			add(tc.input)
		}
	}
	for _, tt := range [][]spanTest{quickSpanTests, quickSpanNFDTests, quickSpanNFCTests} {
		for _, tc := range tt {
			add(tc.input)
		}
	}
	for _, tt := range normTests {
		for _, tc := range tt {
			add(tc.left + tc.right)
		}
	}
	for _, tt := range [][]SegmentTest{segmentTests, segmentTestsK} {
		for _, tc := range tt {
			add(tc.in)
		}
	}
	for _, tc := range nextBoundaryTests {
		add(tc.input)
	}
	for _, tc := range transformTests {
		add(tc.in)
	}
	for _, tc := range exampleCompareTests {
		add(tc.a)
		add(tc.b)
	}
	for _, s := range randomStrings() {
		add(s)
	}
	add(txt_canon)
	add(txt_kr)
	return out
}

// sweepStrings is what the per-rune sweep normalises for r: the rune alone,
// after a starter it may compose with, and followed by two marks that it may
// compose with or be reordered against. For the Hangul and Jamo blocks it
// also puts r between a leading and a trailing Jamo.
func sweepStrings(r rune) []string {
	s := string(r)
	out := []string{s, "a" + s, s + "\u0316\u0301"}
	if r >= 0x1100 && r < 0x1200 || r >= 0xa960 && r < 0xa980 || r >= 0xac00 && r < 0xd800 || r >= 0x3130 && r < 0x3190 {
		out = append(out, "\u1100"+s+"\u11a8")
	}
	return out
}

func sweepRecord(r rune, f Form) []byte {
	var rec record
	s := string(r)
	p := f.PropertiesString(s)
	rec.n("P", int(p.Size()))
	rec.n("", int(p.CCC()))
	rec.n("", int(p.LeadCCC()))
	rec.n("", int(p.TrailCCC()))
	rec.bool("", p.BoundaryBefore())
	rec.bool("", p.BoundaryAfter())
	rec.s("", p.Decomposition())
	for _, x := range sweepStrings(r) {
		rec.s("S", []byte(f.String(x)))
		rec.bool("", f.IsNormalString(x))
	}
	return rec.buf.Bytes()
}

func writeDifferential(t *testing.T, w io.Writer) {
	cs := corpus()
	fmt.Fprintf(w, "/* The strings the C test describes with each form, and the hash of what Go\n")
	fmt.Fprintf(w, " * says about each, form by form. See describe in the generator. */\n")
	fmt.Fprintf(w, "static const NormLit norm_corpus[] = {\n")
	for _, s := range cs {
		fmt.Fprintf(w, "    {%s},\n", clit(s))
	}
	fmt.Fprintf(w, "};\n\nstatic const uint64_t norm_corpus_hash[][4] = {\n")
	for _, s := range cs {
		fmt.Fprint(w, "    {")
		for f := NFC; f <= NFKD; f++ {
			if f > 0 {
				fmt.Fprint(w, ", ")
			}
			fmt.Fprintf(w, "0x%016xU", hash64(describe(f, s)))
		}
		fmt.Fprintln(w, "},")
	}
	fmt.Fprintf(w, "};\n\n")

	// The sweep: one hash per block of 0x1000 runes per form, of the records
	// of every rune in the block, surrogates left out.
	fmt.Fprintf(w, "static const uint64_t norm_sweep_hash[0x110][4] = {\n")
	for blk := rune(0); blk < 0x110; blk++ {
		fmt.Fprint(w, "    {")
		for f := NFC; f <= NFKD; f++ {
			h := fnv.New64a()
			for r := blk << 12; r < (blk+1)<<12; r++ {
				if r >= 0xd800 && r < 0xe000 {
					continue
				}
				h.Write(sweepRecord(r, f))
			}
			if f > 0 {
				fmt.Fprint(w, ", ")
			}
			fmt.Fprintf(w, "0x%016xU", h.Sum64())
		}
		fmt.Fprintln(w, "},")
	}
	fmt.Fprintf(w, "};\n")
}

func TestZZGen(t *testing.T) {
	srcDir := os.Getenv("BURROW_XTEXT_SRC")
	testsDir := os.Getenv("BURROW_XTEXT_TESTS")
	banner := os.Getenv("BURROW_XTEXT_BANNER")
	if srcDir == "" || testsDir == "" || banner == "" {
		t.Skip("run by tools/gen-xtext-norm.sh")
	}
	if dbg := os.Getenv("BURROW_XTEXT_DESCRIBE"); dbg != "" {
		// For chasing a mismatch: what Go says about corpus entry n.
		var n, f int
		fmt.Sscanf(dbg, "%d/%d", &n, &f)
		fmt.Printf("%q\n", describe(Form(f), corpus()[n]))
		return
	}
	writeTables(t, srcDir, banner)

	var b bytes.Buffer
	tb := os.Getenv("BURROW_XTEXT_TEST_BANNER")
	b.WriteString(tb)
	b.WriteString("#ifndef BURROW_TESTS_XTEXT_NORM_TEST_GEN_H\n#define BURROW_TESTS_XTEXT_NORM_TEST_GEN_H\n\n")
	b.WriteString("/* clang-format off */\n\n")
	writeTestData(t, &b)
	writeDifferential(t, &b)
	b.WriteString("\n/* clang-format on */\n\n#endif /* BURROW_TESTS_XTEXT_NORM_TEST_GEN_H */\n")
	write(t, filepath.Join(testsDir, "xtext_norm_test_gen.h"), b.Bytes())
	_ = utf8.RuneError
}
