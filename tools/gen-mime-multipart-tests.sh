#!/bin/sh
# Regenerates tests/mime_multipart_test_gen.h: multipart bodies and what Go's
# mime/multipart makes of them. A reader case is a body and a boundary, and the
# answer is a transcript of every part, its headers, names and body, and how the
# reading ended, for NextPart, for NextRawPart, and for NextPart with only the
# first few bytes of each body read. A form case is a body with a memory limit
# and a GODEBUG setting, and the answer is a transcript of ReadForm: the values,
# the files with where they ended up and what reading them back gives, and what
# is left in the temporary directory before and after RemoveAll. A writer case
# is a list of calls on a Writer and the answer is every error and the bytes
# written. Go's own test bodies are in it, and so are bodies built at random
# from pieces that sit close to a boundary. Where a file name comes out
# differently on Windows, the Windows answer is written down too. The package
# needs unexported fields and internal/godebug, so this copies it into a module
# of its own with a small stand in for godebug that reads the environment on
# every call.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/mime_multipart_test_gen.h"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

src=$(go env GOROOT)/src/mime/multipart
mkdir -p "$tmp/multipart/testdata" "$tmp/internal/godebug"
for f in "$src"/*.go; do
    case $f in *_test.go) ;; *) sed 's#"internal/godebug"#"mp/internal/godebug"#' "$f" > "$tmp/multipart/$(basename "$f")" ;; esac
done
cp "$src/testdata/nested-mime" "$tmp/multipart/testdata/"
cat > "$tmp/go.mod" <<'GO'
module mp

go 1.25
GO
cat > "$tmp/internal/godebug/godebug.go" <<'GO'
package godebug

import (
	"os"
	"strings"
)

type Setting struct{ name string }

func New(name string) *Setting { return &Setting{strings.TrimPrefix(name, "#")} }

func (s *Setting) Name() string { return s.name }

func (s *Setting) IncNonDefault() {}

// The last setting of the name wins, as it does in the runtime.
func (s *Setting) Value() string {
	v := ""
	for _, kv := range strings.Split(os.Getenv("GODEBUG"), ",") {
		if k, val, ok := strings.Cut(kv, "="); ok && k == s.name {
			v = val
		}
	}
	return v
}
GO

cat > "$tmp/multipart/gen_test.go" <<'GO'
package multipart

import (
	"bytes"
	"fmt"
	"io"
	"math"
	"math/rand"
	"mime"
	"net/textproto"
	"os"
	"regexp"
	"sort"
	"strings"
	"testing"
)

func lit(s string) string {
	var b strings.Builder
	b.WriteByte('"')
	col := 0
	for i := 0; i < len(s); i++ {
		if col >= 200 {
			b.WriteString("\"\n\"")
			col = 0
		}
		c := s[i]
		switch {
		case c == '"' || c == '\\':
			b.WriteByte('\\')
			b.WriteByte(c)
			col += 2
		case c == '?':
			b.WriteString(`\077`)
			col += 4
		case c >= ' ' && c <= '~':
			b.WriteByte(c)
			col++
		default:
			fmt.Fprintf(&b, "\\%03o", c)
			col += 4
		}
	}
	b.WriteByte('"')
	return b.String()
}

func qs(s string) string { return "QS(" + lit(s) + ")" }

var tempName = regexp.MustCompile(`/[^ ]*/multipart-[0-9]+`)

// Errors from a temporary file have its random name in them, which becomes
// TMPFILE here and in the C test.
func errs(err error) string {
	if err == nil {
		return "nil"
	}
	return tempName.ReplaceAllString(err.Error(), "TMPFILE")
}

// FNV-1a, so long bodies do not have to be spelled out.
func fnv(b []byte) uint64 {
	h := uint64(14695981039346656037)
	for _, c := range b {
		h ^= uint64(c)
		h *= 1099511628211
	}
	return h
}

func bodyStr(b []byte) string {
	if len(b) <= 200 {
		return fmt.Sprintf("%q", b)
	}
	return fmt.Sprintf("len %d fnv %016x", len(b), fnv(b))
}

func dumpHeader(b *strings.Builder, h textproto.MIMEHeader, indent string) {
	keys := make([]string, 0, len(h))
	for k := range h {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		for _, v := range h[k] {
			fmt.Fprintf(b, "%sh %q: %q\n", indent, k, v)
		}
	}
}

// FileName is filepath.Base of the file name, and filepath.Base splits on a
// backslash too and drops a volume name on Windows. With winNames set, the
// names are worked out the Windows way, with a copy of Go's own code for it,
// so the C test has an answer to check against there.
var winNames bool

func isSep(c byte) bool { return c == '\\' || c == '/' }

func toUpper(c byte) byte {
	if 'a' <= c && c <= 'z' {
		return c - ('a' - 'A')
	}
	return c
}

func pathHasPrefixFold(s, prefix string) bool {
	if len(s) < len(prefix) {
		return false
	}
	for i := 0; i < len(prefix); i++ {
		if isSep(prefix[i]) {
			if !isSep(s[i]) {
				return false
			}
		} else if toUpper(prefix[i]) != toUpper(s[i]) {
			return false
		}
	}
	if len(s) > len(prefix) && !isSep(s[len(prefix)]) {
		return false
	}
	return true
}

func uncLen(path string, prefixLen int) int {
	count := 0
	for i := prefixLen; i < len(path); i++ {
		if isSep(path[i]) {
			count++
			if count == 2 {
				return i
			}
		}
	}
	return len(path)
}

func cutPath(path string) (before, after string, found bool) {
	for i := range path {
		if isSep(path[i]) {
			return path[:i], path[i+1:], true
		}
	}
	return path, "", false
}

func validVolumeNameLen(path string, n int) int {
	for p := path[:n]; p != ""; {
		var part string
		part, p, _ = cutPath(p)
		if part == ".." {
			return 0
		}
	}
	return n
}

func volumeNameLen(path string) int {
	switch {
	case len(path) >= 2 && path[1] == ':':
		return 2
	case len(path) == 0 || !isSep(path[0]):
		return 0
	case pathHasPrefixFold(path, `\\.`) || pathHasPrefixFold(path, `\\?`) || pathHasPrefixFold(path, `\??`):
		switch {
		case len(path) == 3:
			return 3
		case pathHasPrefixFold(path[4:], `UNC`):
			return validVolumeNameLen(path, uncLen(path, len(`\\.\UNC\`)))
		}
		_, rest, ok := cutPath(path[4:])
		if !ok {
			return validVolumeNameLen(path, len(path))
		}
		return validVolumeNameLen(path, len(path)-len(rest)-1)
	case len(path) >= 2 && isSep(path[1]):
		return validVolumeNameLen(path, uncLen(path, 2))
	}
	return 0
}

func winBase(path string) string {
	if path == "" {
		return "."
	}
	for len(path) > 0 && isSep(path[len(path)-1]) {
		path = path[0 : len(path)-1]
	}
	path = path[volumeNameLen(path):]
	i := len(path) - 1
	for i >= 0 && !isSep(path[i]) {
		i--
	}
	if i >= 0 {
		path = path[i+1:]
	}
	if path == "" {
		return `\`
	}
	return path
}

// fileName is Part.FileName, done the Windows way when winNames is set.
func fileName(h textproto.MIMEHeader, name string) string {
	if !winNames {
		return name
	}
	_, params, err := mime.ParseMediaType(h.Get("Content-Disposition"))
	if err != nil || params["filename"] == "" {
		return ""
	}
	return winBase(params["filename"])
}

func readAll(r io.Reader) ([]byte, error) {
	var out []byte
	buf := make([]byte, 512)
	for {
		n, err := r.Read(buf)
		out = append(out, buf[:n]...)
		if err != nil {
			return out, err
		}
	}
}

type oneByte struct{ r io.Reader }

func (o *oneByte) Read(p []byte) (int, error) {
	if len(p) == 0 {
		return o.r.Read(p)
	}
	return o.r.Read(p[:1])
}

func dumpReader(r io.Reader, boundary string, mode int) string {
	mr := NewReader(r, boundary)
	var b strings.Builder
	for i := 0; i < 64; i++ {
		var p *Part
		var err error
		if mode == 1 {
			p, err = mr.NextRawPart()
		} else {
			p, err = mr.NextPart()
		}
		if err != nil {
			fmt.Fprintf(&b, "end %s\n", errs(err))
			return b.String()
		}
		b.WriteString("part\n")
		dumpHeader(&b, p.Header, "")
		fmt.Fprintf(&b, "name %q file %q\n", p.FormName(), fileName(p.Header, p.FileName()))
		if mode == 2 {
			buf := make([]byte, 7)
			n, err := p.Read(buf)
			fmt.Fprintf(&b, "read %s %s\n", bodyStr(buf[:n]), errs(err))
			continue
		}
		data, err := readAll(p)
		fmt.Fprintf(&b, "body %s\nerr %s\n", bodyStr(data), errs(err))
	}
	b.WriteString("more\n")
	return b.String()
}

func countDir(dir string) int {
	names, err := os.ReadDir(dir)
	if err != nil {
		panic(err)
	}
	return len(names)
}

func dumpForm(body, boundary string, maxMemory int64, godebug, dir string) string {
	os.Setenv("GODEBUG", godebug)
	defer os.Setenv("GODEBUG", "")
	mr := NewReader(strings.NewReader(body), boundary)
	mr.tempDir = dir
	var b strings.Builder
	f, err := mr.ReadForm(maxMemory)
	if err != nil {
		fmt.Fprintf(&b, "err %s\nleft %d\n", errs(err), countDir(dir))
		return b.String()
	}
	keys := make([]string, 0, len(f.Value))
	for k := range f.Value {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		fmt.Fprintf(&b, "value %q\n", k)
		for _, v := range f.Value[k] {
			fmt.Fprintf(&b, "  %s\n", bodyStr([]byte(v)))
		}
	}
	keys = keys[:0]
	for k := range f.File {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		fmt.Fprintf(&b, "file %q\n", k)
		for _, fh := range f.File[k] {
			fmt.Fprintf(&b, "  filename %q size %d disk %t shared %t off %d\n", fileName(fh.Header, fh.Filename), fh.Size,
				fh.tmpfile != "", fh.tmpshared, fh.tmpoff)
			dumpHeader(&b, fh.Header, "  ")
			file, err := fh.Open()
			if err != nil {
				fmt.Fprintf(&b, "  open %s\n", errs(err))
				continue
			}
			data, err := readAll(file)
			fmt.Fprintf(&b, "  content %s %s\n", bodyStr(data), errs(err))
			buf := make([]byte, 5)
			n, err := file.ReadAt(buf, 1)
			fmt.Fprintf(&b, "  readat %q %s\n", buf[:n], errs(err))
			pos, err := file.Seek(-3, io.SeekEnd)
			fmt.Fprintf(&b, "  seek %d %s\n", pos, errs(err))
			n, err = file.Read(buf)
			fmt.Fprintf(&b, "  read %q %s\n", buf[:n], errs(err))
			file.Close()
		}
	}
	fmt.Fprintf(&b, "files %d\n", countDir(dir))
	fmt.Fprintf(&b, "removeall %s\n", errs(f.RemoveAll()))
	fmt.Fprintf(&b, "left %d\n", countDir(dir))
	return b.String()
}

// A writer case is a list of calls: B sets the boundary, W writes a field, F
// makes a form file part, C a form field part, H a part with the headers in a,
// one "key: value" per line, D writes a to the newest part, O writes a to the
// oldest one, X closes the writer, and T asks for the content type.
type wop struct{ op byte; a, b string }

func runWriter(ops []wop) string {
	var out bytes.Buffer
	w := NewWriter(&out)
	var parts []io.Writer
	var b strings.Builder
	for _, o := range ops {
		var err error
		switch o.op {
		case 'B':
			err = w.SetBoundary(o.a)
		case 'W':
			err = w.WriteField(o.a, o.b)
		case 'F', 'C', 'H':
			var p io.Writer
			switch o.op {
			case 'F':
				p, err = w.CreateFormFile(o.a, o.b)
			case 'C':
				p, err = w.CreateFormField(o.a)
			default:
				h := make(textproto.MIMEHeader)
				for _, line := range strings.Split(o.a, "\n") {
					if k, v, ok := strings.Cut(line, ": "); ok {
						h.Add(k, v)
					}
				}
				p, err = w.CreatePart(h)
			}
			if err == nil {
				parts = append(parts, p)
			}
		case 'D', 'O':
			if len(parts) == 0 {
				continue
			}
			p := parts[len(parts)-1]
			if o.op == 'O' {
				p = parts[0]
			}
			var n int
			n, err = p.Write([]byte(o.a))
			fmt.Fprintf(&b, "n %d ", n)
		case 'X':
			err = w.Close()
		case 'T':
			fmt.Fprintf(&b, "ct %q\n", w.FormDataContentType())
			continue
		}
		fmt.Fprintf(&b, "%c %s\n", o.op, errs(err))
	}
	fmt.Fprintf(&b, "boundary %q\nout %s\n", w.Boundary(), bodyStr(out.Bytes()))
	return b.String()
}

type gen struct {
	o   *strings.Builder
	rng *rand.Rand
	dir string
}

func (g *gen) reader(body, boundary string, expand bool) {
	in := body
	if expand {
		in = strings.Replace(body, "[longline]", longLine, 1)
	}
	// want[3] is the one byte reader, and it and the Windows answers are only
	// written down when they differ from the answer they fall back on.
	var want, win [4]string
	for w := 0; w < 2; w++ {
		winNames = w == 1
		out := &want
		if winNames {
			out = &win
		}
		for m := 0; m < 3; m++ {
			out[m] = dumpReader(strings.NewReader(in), boundary, m)
		}
		out[3] = dumpReader(&oneByte{strings.NewReader(in)}, boundary, 0)
	}
	winNames = false
	lits := func(w [4]string, base [4]string) string {
		var l [4]string
		for m := range w {
			l[m] = "{NULL, 0}"
			if w[m] != base[m] {
				l[m] = qs(w[m])
			}
		}
		return "{" + strings.Join(l[:], ", ") + "}"
	}
	slowLit := "{NULL, 0}"
	// On Windows the one byte reader is checked against the first of its own
	// Windows answer, the slow answer, the Windows answer for NextPart and the
	// answer for NextPart that is written down.
	base := want
	if want[3] != want[0] {
		slowLit = qs(want[3])
	} else if win[0] != want[0] {
		base[3] = win[0]
	}
	fmt.Fprintf(g.o, "{%s, %s, %t, {%s, %s, %s}, %s, %s},\n", qs(body), qs(boundary), expand,
		qs(want[0]), qs(want[1]), qs(want[2]), slowLit, lits(win, base))
}

func (g *gen) form(body, boundary string, maxMemory int64, godebug string) {
	want := dumpForm(body, boundary, maxMemory, godebug, g.dir)
	winNames = true
	win := dumpForm(body, boundary, maxMemory, godebug, g.dir)
	winNames = false
	winLit := "{NULL, 0}"
	if win != want {
		winLit = qs(win)
	}
	fmt.Fprintf(g.o, "{%s, %s, %dLL, %s, %s, %s},\n", qs(body), qs(boundary), maxMemory, lit(godebug), qs(want), winLit)
}

// The random boundary would change every run, so every case starts with one
// that sticks. A second SetBoundary before the first part still counts.
func (g *gen) writer(ops []wop) {
	ops = append([]wop{{'B', "fixed", ""}}, ops...)
	fmt.Fprintf(g.o, "{%d, {", len(ops))
	for _, o := range ops {
		fmt.Fprintf(g.o, "{'%c', %s, %s}, ", o.op, qs(o.a), qs(o.b))
	}
	fmt.Fprintf(g.o, "}, %s},\n", qs(runWriter(ops)))
}

var boundaries = []string{"MyBoundary", "b", "foo--", "a b", "-", "0016e68ee29c5d515f04cedf6733",
	strings.Repeat("x", 70), "=_", "B"}

func (g *gen) pick(s []string) string { return s[g.rng.Intn(len(s))] }

func (g *gen) randBody(bnd string, big bool) string {
	r := g.rng
	nl := "\r\n"
	if r.Intn(3) == 0 {
		nl = "\n"
	}
	var b strings.Builder
	if r.Intn(2) == 0 {
		b.WriteString(g.pick([]string{"preamble", "", "--" + bnd + "x", "--" + bnd + "--x", " --" + bnd}) + nl)
	}
	nparts := r.Intn(5)
	for i := 0; i < nparts; i++ {
		b.WriteString("--" + bnd)
		if r.Intn(5) == 0 {
			b.WriteString(g.pick([]string{" ", "\t", "  \t", "x", "-"}))
		}
		if r.Intn(10) == 0 {
			b.WriteString(g.pick([]string{"\r\n", "\n"}))
		} else {
			b.WriteString(nl)
		}
		nh := r.Intn(4)
		for j := 0; j < nh; j++ {
			b.WriteString(g.pick([]string{
				`Content-Disposition: form-data; name="f` + fmt.Sprint(r.Intn(4)) + `"`,
				`Content-Disposition: form-data; name="up"; filename="a/b/c.txt"`,
				`Content-Disposition: form-data; name=x; filename="..\\evil"`,
				`Content-Disposition: attachment; filename="doc.pdf"`,
				`Content-Disposition: form-data; name="a"; filename=""`,
				`Content-Disposition: form-data; name*=UTF-8''%C3%A9t%C3%A9`,
				`content-disposition: FORM-DATA; NAME="loud"`,
				`Content-Disposition: form-data; name="bad`,
				`Content-Transfer-Encoding: quoted-printable`,
				`Content-Transfer-Encoding: Quoted-Printable`,
				`Content-Transfer-Encoding: base64`,
				`Content-Type: text/plain; charset=utf-8`,
				`X-Folded: one` + nl + ` two`,
				`X-Empty:`,
				`not a header`,
				`: novalue`,
				`X-Dup: 1`,
				`X-Dup: 2`,
			}) + nl)
		}
		if r.Intn(12) != 0 {
			b.WriteString(nl)
		}
		n := r.Intn(8)
		if big {
			n = 40 + r.Intn(400)
		}
		for j := 0; j < n; j++ {
			b.WriteString(g.pick([]string{
				"a", "hello world", "\r", "\n", "\r\n", "-", "--", "--" + bnd, "\r\n--" + bnd + "x",
				"\n--" + bnd + "-", "=", "=3D", "=\r\n", "=4", "caf=C3=A9", " ", "\t",
				strings.Repeat("z", 100), strings.Repeat("y", 1000), "\x00\xff",
				"--" + bnd[:len(bnd)/2],
			}))
		}
		if r.Intn(10) != 0 {
			b.WriteString(nl)
		}
	}
	switch r.Intn(6) {
	case 0:
		// truncated
	case 1:
		b.WriteString("--" + bnd + "-")
	case 2:
		b.WriteString("--" + bnd + "--")
	default:
		b.WriteString("--" + bnd + "--" + g.pick([]string{"", " ", "\t "}) + nl + g.pick([]string{"", "trailer" + nl, "--" + bnd + nl}))
	}
	return b.String()
}

func (g *gen) randForm(bnd string, big bool) string {
	r := g.rng
	var b strings.Builder
	nparts := r.Intn(8)
	for i := 0; i < nparts; i++ {
		b.WriteString("--" + bnd + "\r\n")
		name := g.pick([]string{"a", "b", "file", "", "a"})
		switch r.Intn(4) {
		case 0:
			fmt.Fprintf(&b, "Content-Disposition: form-data; name=%q\r\n", name)
		case 1:
			fmt.Fprintf(&b, "Content-Disposition: form-data; name=%q; filename=%q\r\n", name,
				g.pick([]string{"x.txt", "dir/y.bin", "", "/", "z"}))
			if r.Intn(2) == 0 {
				b.WriteString("Content-Type: application/octet-stream\r\n")
			}
		case 2:
			fmt.Fprintf(&b, "Content-Disposition: form-data; name=%q; filename=\"q.txt\"\r\nContent-Transfer-Encoding: quoted-printable\r\n", name)
		default:
			b.WriteString("Content-Type: text/plain\r\n")
		}
		b.WriteString("\r\n")
		n := r.Intn(60)
		if big {
			n = r.Intn(3000)
		}
		for j := 0; j < n; j++ {
			b.WriteByte("abc=\r\n0123456789"[r.Intn(16)])
		}
		b.WriteString("\r\n")
	}
	if r.Intn(10) != 0 {
		b.WriteString("--" + bnd + "--\r\n")
	}
	return b.String()
}

func (g *gen) randName() string {
	return g.pick([]string{"f", "file", `a"b`, `c\d`, "e\rf\ng", "ünï", "", "x y"})
}

func (g *gen) randOps() []wop {
	r := g.rng
	var ops []wop
	if r.Intn(4) != 0 {
		ops = append(ops, wop{'B', g.pick([]string{"abc", "", "ungültig", "!", strings.Repeat("x", 70),
			strings.Repeat("x", 71), "bad!ascii!", "my-separator", "with space", "badspace ",
			"(boundary)", "a:b", "'()+_,-./:=?", "tab\there", "@", "semi;colon"}), ""})
	}
	n := r.Intn(8)
	for i := 0; i < n; i++ {
		switch r.Intn(9) {
		case 0:
			ops = append(ops, wop{'W', g.randName(), g.pick([]string{"", "v", "line\r\nline", "--abc"})})
		case 1:
			ops = append(ops, wop{'F', g.randName(), g.pick([]string{"a.txt", `C:\x\y.txt`, `q"t`, "", "n\nl"})})
		case 2:
			ops = append(ops, wop{'C', g.randName(), ""})
		case 3:
			ops = append(ops, wop{'H', g.pick([]string{"", "A: 2\nB: 5\nB: 7\nB: 6\nC: 4\nM: 3\nZ: 1",
				"Content-Type: text/plain\nx-lower: v", "Zed: last\nAlpha: first"}), ""})
		case 4, 5:
			ops = append(ops, wop{'D', g.pick([]string{"", "data", "more data\r\n", strings.Repeat("0123456789", 30)}), ""})
		case 6:
			ops = append(ops, wop{'O', "late", ""})
		case 7:
			ops = append(ops, wop{'T', "", ""})
		default:
			ops = append(ops, wop{'B', "late-boundary", ""})
		}
	}
	if r.Intn(5) != 0 {
		ops = append(ops, wop{'X', "", ""})
	}
	if r.Intn(3) == 0 {
		ops = append(ops, wop{'T', "", ""})
	}
	return ops
}

func TestGen(t *testing.T) {
	dir := t.TempDir()
	g := &gen{o: new(strings.Builder), rng: rand.New(rand.NewSource(2143)), dir: dir}

	fmt.Fprintf(g.o, "static const MpReadCase mp_read_cases[] = {\n")
	for _, tt := range parseTests {
		if tt.name == "round trip" {
			continue
		}
		g.reader(tt.in, tt.sep, false)
	}
	for _, sep := range []string{"\r\n", "\n"} {
		body := testMultipartBody(sep)
		g.reader(strings.Replace(body, longLine, "[longline]", 1), "MyBoundary", true)
	}
	for _, tb := range []string{"Foo\nBar", "Foo\nBar\n", "Foo\r\nBar", "Foo\r\nBar\r\n", "Foo\rBar", "Foo\rBar\r",
		"\x00\x01\x02\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10"} {
		g.reader("--BOUNDARY\r\nContent-Disposition: form-data; name=\"value\"\r\n\r\n"+tb+"\r\n--BOUNDARY--\r\n", "BOUNDARY", false)
	}
	for _, cd := range []string{`form-data; name="foo"`, ` form-data ; name=foo`, `FORM-DATA;name="foo"`,
		` FORM-DATA ; name="foo"`, ` FORM-DATA ; name=foo`, ` FORM-DATA ; filename="foo.txt"; name=foo; baz=quux`,
		` not-form-data ; filename="bar.txt"; name=foo; baz=quux`, `form-data; filename="/a/b/c"`,
		`form-data; filename="a/b/"`, `form-data; filename="///"`, `form-data; filename="."`, `form-data; filename=".."`,
		`form-data; filename="C:\\dir\\a.txt"`, `form-data; filename="c:"`, `form-data; filename="1:x"`,
		`form-data; filename="a/b\\c"`, `form-data; filename="\\\\host\\share"`,
		`form-data; filename="\\\\host\\share\\f.txt"`, `form-data; filename="\\\\..\\s"`,
		`form-data; filename="\\\\?\\c:\\f"`, `form-data; filename="\\\\.\\UNC\\h\\s\\f"`,
		`form-data; filename="\\\\.\\unc\\h"`, `form-data; filename="\\??\\x"`, `form-data; filename="\\\\."`} {
		g.reader("--b\r\nContent-Disposition: "+cd+"\r\n\r\nx\r\n--b--\r\n", "b", false)
	}
	g.reader(strings.ReplaceAll("\nThis is a multi-part message.  This line is ignored.\n--MyBoundary\nfoo-bar: baz\n\nOh no, premature EOF!\n", "\n", "\r\n"), "MyBoundary", false)
	g.reader(strings.ReplaceAll("\nThis is a multi-part message.  This line is ignored.\n--MyBoundary\nfoo-bar: baz\n\nOh no, premature EOF!\n--MyBoundary-", "\n", "\r\n"), "MyBoundary", false)
	g.reader("\n--Apple-Mail-2-292336769\nContent-Transfer-Encoding: 7bit\nContent-Type: text/plain;\n\tcharset=US-ASCII;\n\tdelsp=yes;\n\tformat=flowed\n\nI'm finding the same thing happening on my system (10.4.1).\n\n\n--Apple-Mail-2-292336769\nContent-Transfer-Encoding: quoted-printable\nContent-Type: text/html;\n\tcharset=ISO-8859-1\n\n<HTML><BODY>I'm finding the same thing =\nhappening on my system (10.4.1).=A0 But I built it with XCode =\n2.0.</BODY></=\nHTML>=\n\r\n--Apple-Mail-2-292336769--\n", "Apple-Mail-2-292336769", false)
	for _, cte := range []string{"quoted-printable", "Quoted-PRINTABLE"} {
		g.reader("--0016e68ee29c5d515f04cedf6733\r\nContent-Type: text/plain; charset=ISO-8859-1\r\nContent-Disposition: form-data; name=text\r\nContent-Transfer-Encoding: "+cte+"\r\n\r\nwords words words words words words words words words words words words wor=\r\nds words words words words words words words words words words words words =\r\nwords words words words words words words words words words words words wor=\r\nds words words words words words words words words words words words words =\r\nwords words words words words words words words words\r\n--0016e68ee29c5d515f04cedf6733\r\nContent-Type: text/plain; charset=ISO-8859-1\r\nContent-Disposition: form-data; name=submit\r\n\r\nSubmit\r\n--0016e68ee29c5d515f04cedf6733--", "0016e68ee29c5d515f04cedf6733", false)
	}
	g.reader(strings.ReplaceAll("--0016e68ee29c5d515f04cedf6733\nContent-Type: text/plain; charset=\"utf-8\"\nContent-Transfer-Encoding: quoted-printable\n\n<div dir=3D\"ltr\">Hello World.</div>\n--0016e68ee29c5d515f04cedf6733\nContent-Type: text/plain; charset=\"utf-8\"\nContent-Transfer-Encoding: quoted-printable\n\n<div dir=3D\"ltr\">Hello World.</div>\n--0016e68ee29c5d515f04cedf6733--", "\n", "\r\n"), "0016e68ee29c5d515f04cedf6733", false)
	nested, err := os.ReadFile("testdata/nested-mime")
	if err != nil {
		t.Fatal(err)
	}
	g.reader(string(nested), "e89a8ff1c1e83553e304be640612", false)
	g.reader("", "", false)
	g.reader("--\r\n", "", false)
	g.reader("", "b", false)
	g.reader("--b", "b", false)
	g.reader("--b--", "b", false)
	g.reader("--b\r\n\r\n--b--", "b", false)
	g.reader("--b\n\nbody\n--b--\n", "b", false)
	g.reader("--b\r\n\r\nbody\n--b--\r\n", "b", false)
	g.reader("--b\r\n\r\nbody\r\n--b\r\n\r\n\r\n--b--", "b", false)
	g.reader("--b\r\n\r\nbody\r\n--bb\r\n--b--", "b", false)
	g.reader("--b\r\n\r\nx\r\n\r\n--b--", "b", false)
	g.reader("--b\r\n\r\nx\r\ngarbage\r\n--b--", "b", false)
	g.reader("--b\r\nX: "+strings.Repeat("v", 5000)+"\r\n\r\nx\r\n--b--", "b", false)
	g.reader("--b\r\n\r\n"+strings.Repeat("\r\n-", 3000)+"\r\n--b--", "b", false)
	g.reader("--"+strings.Repeat("x", 70)+"\r\n\r\n"+strings.Repeat("a", 4090)+"\r\n--"+strings.Repeat("x", 69)+"\r\n--"+strings.Repeat("x", 70)+"--", strings.Repeat("x", 70), false)
	for i := 0; i < 360; i++ {
		bnd := g.pick(boundaries)
		g.reader(g.randBody(bnd, i%12 == 0), bnd, false)
	}
	fmt.Fprintf(g.o, "};\n\n")

	fmt.Fprintf(g.o, "static const MpFormCase mp_form_cases[] = {\n")
	const (
		fileaContents = "This is a test file."
		filebContents = "Another test file."
	)
	crlf := func(s string) string { return strings.ReplaceAll(s, "\n", "\r\n") }
	for _, mm := range []int64{25, 0, 1 << 20, math.MaxInt64, -1, 38, 39, -10 << 20, math.MaxInt64 - (10 << 20)} {
		g.form(crlf(message), boundary, mm, "")
		g.form(crlf(messageWithFileWithoutName), boundary, mm, "")
		g.form(crlf(messageWithFileName), boundary, mm, "")
		g.form(crlf(messageWithTextContentType), boundary, mm, "")
	}
	g.form(crlf(message), boundary, 0, "multipartfiles=distinct")
	g.form(crlf(message), boundary, 0, "multipartfiles=distinct,multipartfiles=combined")
	g.form(crlf(message), boundary, 0, "multipartmaxparts=3")
	g.form(crlf(message), boundary, 0, "multipartmaxparts=4")
	g.form(crlf(message), boundary, 0, "multipartmaxparts=-1")
	g.form(crlf(message), boundary, 0, "multipartmaxparts=x")
	g.form(crlf(message), boundary, 0, "multipartmaxheaders=3")
	g.form(crlf(message), boundary, 0, "multipartmaxheaders=4")
	g.form(crlf(message), boundary, 0, "multipartmaxheaders=0")
	g.form("\n-----------------------------8d345eef0d38dc9\nContent-Disposition: form-data; name=\"version\"\n\n171\n-----------------------------8d345eef0d38dc9--", "---------------------------8d345eef0d38dc9", 32<<20, "")
	gds := []string{"", "", "", "multipartfiles=distinct", "multipartmaxparts=2", "multipartmaxheaders=2"}
	for i := 0; i < 200; i++ {
		bnd := g.pick(boundaries[:4])
		mm := []int64{0, 10, 100, 1000, 5000, 1 << 20, -1}[g.rng.Intn(7)]
		g.form(g.randForm(bnd, i%8 == 0), bnd, mm, g.pick(gds))
	}
	fmt.Fprintf(g.o, "};\n\n")

	fmt.Fprintf(g.o, "static const MpWriterCase mp_writer_cases[] = {\n")
	g.writer([]wop{{'F', "myfile", "my-file.txt"}, {'D', "my file contents", ""}, {'W', "key", "val"}, {'O', "val", ""}, {'X', "", ""}})
	g.writer([]wop{{'B', "MIMEBOUNDARY", ""}, {'H', "A: 2\nB: 5\nB: 7\nB: 6\nC: 4\nM: 3\nZ: 1", ""}, {'D', "foo", ""}, {'X', "", ""}})
	for _, b := range []string{"abc", "", "ungültig", "!", strings.Repeat("x", 70), strings.Repeat("x", 71),
		"bad!ascii!", "my-separator", "with space", "badspace ", "(boundary)"} {
		g.writer([]wop{{'B', b, ""}, {'T', "", ""}, {'X', "", ""}})
	}
	for _, f := range [][2]string{{"somefield", "somefile.txt"}, {`field"withquotes"`, "somefile.txt"},
		{`somefield`, `somefile"withquotes".txt`}, {`somefield\withbackslash`, "somefile.txt"},
		{"somefield", `somefile\withbackslash.txt`}, {"a\rb\nc", "e\rf\ng"}} {
		g.writer([]wop{{'B', "b", ""}, {'F', f[0], f[1]}, {'X', "", ""}})
	}
	g.writer([]wop{{'X', "", ""}, {'X', "", ""}})
	g.writer([]wop{{'B', "b", ""}, {'X', "", ""}, {'C', "after", ""}, {'D', "x", ""}, {'X', "", ""}})
	for i := 0; i < 150; i++ {
		g.writer(g.randOps())
	}
	fmt.Fprintf(g.o, "};\n")

	if err := os.WriteFile(os.Getenv("MP_OUT"), []byte(g.o.String()), 0o644); err != nil {
		t.Fatal(err)
	}
}
GO

cp "$src/multipart_test.go" "$src/formdata_test.go" "$tmp/multipart/"
body="$tmp/body.h"
(cd "$tmp/multipart" && MP_OUT="$body" go test -count=1 -run '^TestGen$' . > "$tmp/log" 2>&1) || { cat "$tmp/log"; exit 1; }

{
    echo "/* Generated by tools/gen-mime-multipart-tests.sh from Go $(go env GOVERSION). Do not edit. */"
    echo
    cat "$body"
} > "$out"
if command -v clang-format >/dev/null 2>&1; then
    clang-format -i "$out"
fi
