#!/bin/sh
# Regenerates tests/mime_type_test_gen.h: a script of steps on mime's extension
# table and what Go's mime package answers at each one. A step empties the
# table or puts the built in types back, loads a globs2 or mime.types file,
# calls AddExtensionType or setExtensionType, or asks TypeByExtension or
# ExtensionsByType something. The C test replays the steps and checks every
# answer. Go's own test cases are in it, and so are files built at random from
# pieces, which is where the parsers of the two file formats get checked. The
# loaders are unexported in Go, so this copies the package next to a test file
# that drives them. Nothing here depends on the machine, so it runs wherever go
# does.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/mime_type_test_gen.h"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

src=$(go env GOROOT)/src/mime
for f in "$src"/*.go; do
    case $f in *_test.go) ;; *) cp "$f" "$tmp"/ ;; esac
done
mkdir "$tmp/testdata"
cp "$src"/testdata/test.types "$src"/testdata/test.types.globs2 "$tmp/testdata/"
cat > "$tmp/stub_test.go" <<'GO'
package mime

func init() { testInitMime = func() {} }
GO

cat > "$tmp/gen_test.go" <<'GO'
package mime

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"testing"
)

func lit(s string) string {
	var b strings.Builder
	b.WriteByte('"')
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == '"' || c == '\\':
			b.WriteByte('\\')
			b.WriteByte(c)
		case c == '?':
			b.WriteString(`\077`)
		case c >= ' ' && c <= '~':
			b.WriteByte(c)
		default:
			fmt.Fprintf(&b, "\\%03o", c)
		}
	}
	b.WriteByte('"')
	return b.String()
}

func qs(s string) string { return "QS(" + lit(s) + ")" }

func errText(err error) string {
	if err == nil {
		return ""
	}
	return err.Error()
}

// The error from a globs2 file has the path in it, so only whether there was
// one is kept.
func failed(err error) string {
	if err == nil {
		return ""
	}
	return "failed"
}

type rnd uint64

func (x *rnd) next() uint64 {
	*x = *x*6364136223846793005 + 1442695040888963407
	return uint64(*x >> 33)
}

type gen struct {
	o   *os.File
	dir string
	n   int
}

func (g *gen) step(op, a, b, want, err string) {
	fmt.Fprintf(g.o, "{%s, %s, %s, %s, %s},\n", op, qs(a), qs(b), qs(want), lit(err))
	g.n++
}

func (g *gen) reset(builtin bool) {
	if builtin {
		setMimeTypes(builtinTypesLower, builtinTypesLower)
		g.step("MT_RESET_BUILTIN", "", "", "", "")
	} else {
		setMimeTypes(map[string]string{}, map[string]string{})
		g.step("MT_RESET_EMPTY", "", "", "", "")
	}
}

func (g *gen) file(content string) string {
	p := filepath.Join(g.dir, fmt.Sprintf("f%d", g.n))
	if err := os.WriteFile(p, []byte(content), 0o644); err != nil {
		panic(err)
	}
	return p
}

func (g *gen) globs(content string) {
	g.step("MT_GLOBS", content, "", "", failed(loadMimeGlobsFile(g.file(content))))
}

func (g *gen) types(content string) {
	loadMimeFile(g.file(content))
	g.step("MT_TYPES", content, "", "", "")
}

func (g *gen) set(ext, typ string) {
	g.step("MT_SET", ext, typ, "", errText(setExtensionType(ext, typ)))
}

func (g *gen) add(ext, typ string) {
	g.step("MT_ADD", ext, typ, "", errText(AddExtensionType(ext, typ)))
}

func (g *gen) typ(ext string) {
	g.step("MT_TYPE", ext, "", TypeByExtension(ext), "")
}

// The list comes back joined by newlines, with a newline after each one, so
// that no extensions and one empty extension are different.
func (g *gen) exts(typ string) {
	l, err := ExtensionsByType(typ)
	var b strings.Builder
	for _, e := range l {
		b.WriteString(e)
		b.WriteByte('\n')
	}
	g.step("MT_EXTS", typ, "", b.String(), errText(err))
}

var probeExts = []string{
	"", ".", ".html", ".HTML", ".HtMl", ".htm", ".txt", ".TXT", ".png", ".PNG",
	".js", ".mjs", ".json", ".svg", ".tar", ".unknown", "html", ".T1", ".t1",
	".t2", ".T2", ".t3", ".t4", ".T4", ",v", "~", ".foo?ar", ".foo*r",
	".foo[1-3]", ".abc", ".ABC", ".Abc", ".def", ".DEF", ".g", ".G", ".x1",
	".tt", ".TT", ".é", ".É", ".été", ".ÉTÉ",
	".K", ".k", ".K", ".İ", ".i̇", ".café",
	".averyveryveryveryveryveryveryveryveryveryveryveryveryverylongextension",
	".AVERYVERYVERYVERYVERYVERYVERYVERYVERYVERYVERYVERYVERYVERYLONGEXTENSION",
}

var probeTypes = []string{
	"text/html", "TEXT/HTML", "text/html; charset=utf-8", "image/png",
	"image/jpeg", "application/octet-stream", "text/plain", "text/xml",
	"audio/ogg", "application/postscript", "x/unknown", "application/test",
	"text/test", "document/test", "example/test", "text/x-a", "image/b",
	"application/c", "image/x-foo", "application/x-a", "", "bad", "text/",
	"text/html; x", "text/html; charset=utf-8; charset=latin1",
}

func (g *gen) probe() {
	for _, e := range probeExts {
		g.typ(e)
	}
	for _, t := range probeTypes {
		g.exts(t)
	}
}

var globsPieces = []string{
	"50", "10", "#", ":", ":", ":", "text/plain", "image/X-Foo",
	"application/x-a", "*.", "*.", "*", "abc", "ABC", "Tt", "?", "[", "~",
	",v", " ", "\t", "\n", "\n", "\n", "\r\n", "; charset=latin1", "text/",
	".", "é", "=", "\"", "text/x-a; q=\"1\"", "cs", "bad type",
}

var typesPieces = []string{
	"text/x-a", "image/b", "application/C", "text/x-a; q=1", " ", " ",
	"\t", "\n", "\n", "\n", "#", "abc", "Def", "g", "x1", "\r\n", "text/",
	"é", " ", " ", "bad", "text/plain; charset=", "Tt", "K",
}

func build(x *rnd, pieces []string, max uint64) string {
	var b strings.Builder
	n := x.next() % (max + 1)
	for i := uint64(0); i < n; i++ {
		b.WriteString(pieces[x.next()%uint64(len(pieces))])
	}
	return b.String()
}

func TestGen(t *testing.T) {
	once.Do(initMime)
	o, err := os.Create(os.Getenv("MIME_GEN_OUT"))
	if err != nil {
		t.Fatal(err)
	}
	defer o.Close()
	p := func(f string, a ...any) { fmt.Fprintf(o, f, a...) }
	p("/* Generated by tools/gen-mime-type-tests.sh from Go %s. Do not edit. */\n\n", runtime.Version())
	p("static const MimeTypeStep mime_type_steps[] = {\n")
	g := &gen{o: o, dir: t.TempDir()}

	// The built in table on its own.
	g.reset(true)
	g.probe()
	var keys []string
	for k := range builtinTypesLower {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		g.typ(k)
		g.typ(strings.ToUpper(k))
		g.exts(builtinTypesLower[k])
	}

	// TestTypeByExtension_LocalData.
	g.reset(false)
	g.set(".foo", "x/foo")
	g.set(".bar", "x/bar")
	g.set(".Bar", "x/bar; capital=1")
	for _, e := range []string{".foo", ".bar", ".Bar", ".sdlkfjskdlfj", ".t1"} {
		g.typ(e)
	}
	g.exts("x/bar")

	// TestTypeByExtensionCase.
	g.reset(false)
	g.set(".TEST", "test/test; WAS=ALLCAPS")
	g.set(".tesT", "test/test; charset=iso-8859-1")
	for _, e := range []string{".tesT", ".TEST", ".TesT", ".test"} {
		g.typ(e)
	}
	g.exts("test/test")

	// TestAddExtensionType_TextMIMEWithParamsDefaultCharset, and more.
	g.reset(false)
	g.add(".regtxt", "text/x-reg; myparam=myvalue")
	g.typ(".regtxt")
	g.add("regtxt", "text/plain")
	g.add("", "text/plain")
	g.add(".x", "bad type")
	g.add(".x", "text/html; x")
	g.add(".x", "")
	g.add(".e", "text/plain; charset=")
	g.add(".f", "Text/Plain")
	g.add(".g", "text/plain; CHARSET=latin1")
	g.add(".h", "text/plain; charset=\"\"")
	g.add(".i", "image/PNG; q=1")
	g.add(".I", "image/png")
	g.add(".ÉTÉ", "text/x-e")
	g.add(".K", "text/x-k")
	g.add(".j", "text/plain; title*=utf-8''%E2%82%AC")
	g.add(".", "text/plain")
	for _, e := range []string{".e", ".f", ".g", ".h", ".i", ".I", ".x", "été", ".été", ".ÉTÉ", ".K", ".k", ".K", ".j", "."} {
		g.typ(e)
	}
	for _, ty := range []string{"text/plain", "image/png", "text/x-e", "text/x-k", "text/x-reg"} {
		g.exts(ty)
	}

	// TestExtensionsByType.
	g.reset(false)
	g.set(".gif", "image/gif")
	g.set(".a", "foo/letter")
	g.set(".b", "foo/letter")
	g.set(".B", "foo/letter")
	g.set(".PNG", "image/png")
	for _, ty := range []string{"image/gif", "image/png", "foo/letter", "x/unknown"} {
		g.exts(ty)
	}

	// TestTypeByExtensionUNIX, with Go's files, and a globs2 file that is not
	// there.
	g.reset(true)
	testGlobs, _ := os.ReadFile("testdata/test.types.globs2")
	testTypes, _ := os.ReadFile("testdata/test.types")
	g.globs(string(testGlobs))
	g.types(string(testTypes))
	g.probe()
	g.step("MT_GLOBS_MISSING", "", "", "", failed(loadMimeGlobsFile(filepath.Join(g.dir, "missing"))))

	// Files from pieces.
	for i := uint64(1); i <= 200; i++ {
		x := rnd(i * 104729)
		g.reset(x.next()%2 == 0)
		if x.next()%2 == 0 {
			g.globs(build(&x, globsPieces, 120))
		}
		if x.next()%2 == 0 {
			g.types(build(&x, typesPieces, 120))
		}
		g.probe()
	}

	p("};\n\n#define MIME_TYPE_STEP_COUNT %d\n", g.n)
}
GO

(cd "$tmp" && go mod init mimegen >/dev/null 2>&1 &&
    MIME_GEN_OUT="$out.tmp" go test -count=1 -run '^TestGen$' . >/dev/null)
mv "$out.tmp" "$out"
if command -v clang-format >/dev/null 2>&1; then
    clang-format -i "$out"
fi
